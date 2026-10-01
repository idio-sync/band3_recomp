#include "src/Render/native_view.h"

#include "src/Render/capture_file.h"
#include "src/Render/gpu_view.h"
#include "src/Render/png_writer.h"
#include "src/settings.h"

#include <imgui.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// See native_view.h.

namespace band3::render {
namespace {
// `drawn`: what drew it, the last line
std::string Describe(const FrameCapture& fc, const std::string& drawn) {
    std::map<std::string, int> blends;
    uint32_t skinned = 0, verts = 0, tris = 0;
    for (const DrawItem& d : fc.draws) {
        blends["blend " + std::to_string(d.blend) + " z " + std::to_string(d.z_mode)]++;
        if (!d.bones.empty()) skinned++;
        verts += uint32_t(d.geom->verts.size());
        tris += uint32_t(d.geom->indices.size() / 3);
    }
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "frame %llu: %zu draws (%u skinned, %u verts, %u tris) from %u cameras\n"
                  "skipped: %u render-target, %u velocity, %u no geometry; %u mutable\n"
                  "%u multimesh instances, %u particles\n"
                  "textures: %u decoded, %u other formats; cache hits geom %u tex %u\n",
                  static_cast<unsigned long long>(fc.frame), fc.draws.size(), skinned, verts,
                  tris, fc.cams, fc.skipped_target, fc.skipped_velocity, fc.skipped_no_geom,
                  fc.mutable_meshes, fc.multimesh_instances, fc.particles, fc.textured,
                  fc.untextured_format, fc.geom_cached, fc.tex_cached);
    std::string s = buf + drawn;
    for (auto& [k, n] : blends) s += "  " + k + ": " + std::to_string(n) + "\n";
    return s;
}

std::string DescribeRaster(const RasterStats& rs) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "raster: %u draws, %u tris on screen, %u pixels, %.1f ms\n",
                  rs.draws, rs.triangles, rs.pixels, rs.ms);
    return buf;
}

std::string DescribeGpu(const GpuStats& gs) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "gpu: %u draws (%u not drawn yet), %u uploads, %.1f ms (%.1f ms after submit)\n",
                  gs.draws, gs.skipped, gs.uploads, gs.ms, gs.wait_ms);
    return buf;
}

// ---------------------------------------------------------------------------
// the worker that rasterizes the newest capture

class Renderer {
 public:
    static Renderer& Get() {
        static Renderer r;
        return r;
    }

    void AddUser() {
        std::lock_guard lock(mutex_);
        if (users_++ == 0) {
            AcquireCapture();
            stop_ = false;
            thread_ = std::thread([this] { Run(); });
        }
    }

    void RemoveUser() {
        std::thread t;
        {
            std::lock_guard lock(mutex_);
            if (users_ == 0 || --users_ > 0) return;
            ReleaseCapture();
            stop_ = true;
            t = std::move(thread_);
        }
        if (t.joinable()) t.join();
    }

    void SetOptions(const RasterOptions& o) {
        std::lock_guard lock(mutex_);
        options_ = o;
        options_changed_ = true;
    }

    // the GPU when the device is there, else the CPU
    void SetGpu(bool gpu) {
        std::lock_guard lock(mutex_);
        options_changed_ |= gpu_ != gpu;
        gpu_ = gpu;
    }

    void SetDumpPath(std::string path) {
        std::lock_guard lock(mutex_);
        dump_path_ = std::move(path);
    }

    // copies the newest picture if it is newer than `frame`
    bool Take(uint64_t& frame, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h,
              std::string& stats) {
        std::lock_guard lock(mutex_);
        stats = stats_;
        if (image_serial_ == frame || image_.empty()) return false;
        frame = image_serial_;
        rgba = image_;
        w = image_w_;
        h = image_h_;
        return true;
    }

 private:
    void Run() {
        uint64_t last_frame = 0;
        auto last_dump = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        std::vector<uint32_t> rgba;
        while (true) {
            RasterOptions o;
            std::string dump;
            bool changed, gpu;
            {
                std::lock_guard lock(mutex_);
                if (stop_) return;
                o = options_;
                gpu = gpu_;
                changed = options_changed_;
                options_changed_ = false;
                dump = dump_path_;
            }
            auto cap = LatestCapture();
            if (!cap || (cap->frame == last_frame && !changed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            last_frame = cap->frame;
            // dumping saves captures for tools/native_view_replay and leaves
            // the drawing to it
            if (!dump.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_dump >= std::chrono::seconds(5) && !cap->draws.empty()) {
                    last_dump = now;
                    char name[32];
                    std::snprintf(name, sizeof(name), ".%03u.cap", dump_count_++ % 60);
                    SaveCapture(dump + name, *cap);
                    if (FILE* f = std::fopen((dump + name + ".txt").c_str(), "w")) {
                        std::fputs(Describe(*cap, {}).c_str(), f);
                        std::fclose(f);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // RenderFrame fails (and stays failed) without a device, and the
            // CPU draws instead
            GpuStats gs;
            const std::string stats =
                gpu && GpuRenderer::Get().RenderFrame(*cap, o, rgba, gs)
                    ? Describe(*cap, DescribeGpu(gs))
                    : Describe(*cap, DescribeRaster(Rasterize(*cap, o, rgba)));
            {
                std::lock_guard lock(mutex_);
                image_ = rgba;
                image_w_ = o.width;
                image_h_ = o.height;
                image_serial_++;
                stats_ = stats;
            }
        }
    }

    std::mutex mutex_;
    std::thread thread_;
    int users_ = 0;
    bool stop_ = false;
    RasterOptions options_;
    bool gpu_ = false;
    bool options_changed_ = false;
    std::string dump_path_;
    std::vector<uint32_t> image_;
    uint32_t image_w_ = 0, image_h_ = 0;
    uint64_t image_serial_ = 0;
    uint32_t dump_count_ = 0;
    std::string stats_;
};

bool g_dumping = false;

}  // namespace

NativeViewDialog::NativeViewDialog(rex::ui::ImGuiDrawer* imgui_drawer, DrawerGetter drawer)
    : rex::ui::ImGuiDialog(imgui_drawer), drawer_(std::move(drawer)) {}

NativeViewDialog::~NativeViewDialog() {
    if (visible_) Renderer::Get().RemoveUser();
}

bool NativeViewDialog::WantsGpu() {
    // here, on the UI thread, where SDL wants its video started
    return REXCVAR_GET(native_view_backend) == "gpu" && GpuRenderer::Get().Init();
}

void NativeViewDialog::Toggle() {
    visible_ = !visible_;
    if (visible_) {
        Renderer::Get().SetOptions(options_);
        Renderer::Get().SetGpu(WantsGpu());
        Renderer::Get().AddUser();
    } else {
        Renderer::Get().RemoveUser();
        texture_.reset();
    }
}

void NativeViewDialog::OnDraw(ImGuiIO& io) {
    (void)io;
    if (!visible_) return;
    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Native view (experimental)", &open)) {
        bool changed = false;
        changed |= ImGui::Checkbox("Textures", &options_.textures);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Lighting", &options_.lighting);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Skinning", &options_.skinning);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Blending", &options_.blending);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Clear depth per camera", &options_.clear_depth_per_camera);
        int size = options_.width >= 1280 ? 2 : options_.width >= 960 ? 1 : 0;
        if (ImGui::Combo("Size", &size, "640x360\0960x540\01280x720\0")) {
            const uint32_t widths[] = {640, 960, 1280};
            options_.width = widths[size];
            options_.height = options_.width * 9 / 16;
            changed = true;
        }
        if (changed) Renderer::Get().SetOptions(options_);
        // native_view_backend, which F4 can change too
        int backend = REXCVAR_GET(native_view_backend) == "gpu" ? 1 : 0;
        if (ImGui::Combo("Backend", &backend, "CPU\0GPU\0"))
            rex::cvar::SetFlagByName("native_view_backend", backend ? "gpu" : "cpu");
        const bool gpu = WantsGpu();
        Renderer::Get().SetGpu(gpu);
        if (backend == 1 && !gpu) {
            ImGui::SameLine();
            ImGui::TextUnformatted("(no GPU device, drawing on the CPU; see the log)");
        }

        std::vector<uint32_t> rgba;
        uint32_t w = 0, h = 0;
        std::string stats;
        if (Renderer::Get().Take(texture_frame_, rgba, w, h, stats)) {
            if (auto* drawer = drawer_ ? drawer_() : nullptr) {
                texture_ = drawer->CreateTexture(w, h, rex::ui::ImmediateTextureFilter::kLinear,
                                                 false,
                                                 reinterpret_cast<const uint8_t*>(rgba.data()));
            }
        }
        ImGui::TextUnformatted(stats.c_str());
        if (texture_) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float aspect = float(texture_->height) / float(texture_->width);
            ImVec2 size_px(avail.x, avail.x * aspect);
            if (size_px.y > avail.y && avail.y > 0) size_px = ImVec2(avail.y / aspect, avail.y);
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                             reinterpret_cast<uintptr_t>(texture_.get()))),
                         size_px);
        }
    }
    ImGui::End();
    if (!open) Toggle();
}

void StartDumpIfRequested() {
    const char* path = std::getenv("BAND3_NATIVE_VIEW_DUMP");
    if (!path || !*path || g_dumping) return;
    g_dumping = true;
    REXLOG_INFO("native view: writing {} every two seconds", path);
    Renderer::Get().SetDumpPath(path);
    Renderer::Get().SetOptions(RasterOptions{});
    Renderer::Get().AddUser();
}

void StopNativeView() {
    if (g_dumping) {
        g_dumping = false;
        Renderer::Get().RemoveUser();
    }
}

}  // namespace band3::render
