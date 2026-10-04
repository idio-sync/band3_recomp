#include "src/Render/native_view.h"

#include "src/Render/capture_file.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/gpu_view.h"
#include "src/Render/png_writer.h"
#include "src/Render/post_model.h"
#include "src/Render/present_model.h"
#include "src/crash_trace.h"
#include "src/settings.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/window.h>
#ifdef _WIN32
#include <rex/ui/d3d12/d3d12_presenter.h>

#include "src/Render/shaders/present_shaders.gen.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "frame %llu: %zu draws (%u skinned, %u verts, %u tris) from %u cameras\n"
                  "skipped: %u render-target, %u velocity, %u shadow, %u other pass, "
                  "%u no geometry; %u mutable\n"
                  "%u multimesh instances, %u particles; material passes after a mesh's "
                  "first %u, without a material %u; DrawFaces outside a DrawShowing %u\n"
                  "textures: %u decoded, %u other formats; cache hits geom %u tex %u\n"
                  "texture passes: %u drawn, %u carried, %u empty, %u unbalanced; render "
                  "targets sampled %u, missing %u, their pass's draws left out %u; "
                  "snapshots %u\n",
                  static_cast<unsigned long long>(fc.frame), fc.draws.size(), skinned, verts,
                  tris, fc.cams, fc.skipped_target, fc.skipped_velocity, fc.skipped_shadow,
                  fc.skipped_draw_mode, fc.skipped_no_geom,
                  fc.mutable_meshes, fc.multimesh_instances, fc.particles, fc.later_passes,
                  fc.skipped_no_mat, fc.faces_elsewhere, fc.textured,
                  fc.untextured_format, fc.geom_cached, fc.tex_cached, fc.passes_own,
                  fc.passes_carried, fc.passes_empty, fc.passes_unbalanced, fc.rt_sampled,
                  fc.rt_missing, fc.rt_filtered, fc.rt_snapshots);
    std::string s = buf;
    // even/odd rendering: a post frame's capture has the world frame's world
    if (fc.composed) {
        std::snprintf(buf, sizeof(buf), "world from game frame %llu, overlay from %llu\n",
                      static_cast<unsigned long long>(fc.world_frame),
                      static_cast<unsigned long long>(fc.game_frame));
        s += buf;
    }
    s += drawn;
    for (auto& [k, n] : blends) s += "  " + k + ": " + std::to_string(n) + "\n";
    return s;
}

std::string DescribeRaster(const RasterStats& rs) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "raster: %u draws (%u texture passes), %u tris on screen, %u pixels, %.1f ms; "
                  "%u sampled a render target nothing drew\n",
                  rs.draws, rs.passes, rs.triangles, rs.pixels, rs.ms, rs.rt_missing);
    return buf;
}

std::string DescribeGpu(const GpuStats& gs) {
    char buf[240];
    std::snprintf(buf, sizeof(buf),
                  "gpu: %u draws (%u texture passes, %u not drawn yet), %u uploads, %.1f ms "
                  "(%.1f ms after submit); %u sampled a render target nothing drew\n",
                  gs.draws, gs.passes, gs.skipped, gs.uploads, gs.ms, gs.wait_ms, gs.rt_missing);
    return buf;
}

int64_t Nanoseconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// the worker that rasterizes the newest capture

// native_max_height: a window's picture taller than it is drawn that tall,
// its width in proportion, and the presenter scales it up to fill the
// window's (present.hlsl filters a picture that isn't 1:1)
void CapHeight(RasterOptions& o) {
    const int32_t max_h = REXCVAR_GET(native_max_height);
    if (max_h <= 0 || o.height <= uint32_t(max_h)) return;
    o.width = std::max<uint32_t>(
        1, uint32_t((uint64_t(o.width) * uint32_t(max_h) + o.height / 2) / o.height));
    o.height = uint32_t(max_h);
}

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
        WakeCaptureWaiters();
        paint_cv_.notify_all();
        if (t.joinable()) t.join();
    }

    void SetOptions(const RasterOptions& o) {
        {
            std::lock_guard lock(mutex_);
            options_ = o;
            options_changed_ = true;
        }
        WakeCaptureWaiters();
    }

    // the GPU when the device is there, else the CPU
    void SetGpu(bool gpu) {
        {
            std::lock_guard lock(mutex_);
            if (gpu_ == gpu) return;
            options_changed_ = true;
            gpu_ = gpu;
        }
        WakeCaptureWaiters();
    }

    void SetDumpPath(std::string path) {
        std::lock_guard lock(mutex_);
        dump_path_ = std::move(path);
    }

    // F9's window shows each frame's RGBA, which the presenter's zero-copy
    // frames otherwise never have
    void SetDialogOpen(bool open) {
        std::lock_guard lock(mutex_);
        dialog_open_ = open;
    }

    // The presenter (renderer = native) as a user. While it is one, its size
    // is the size drawn at, whatever the window or the live view set; and on
    // the zero-copy path each frame goes into one of the outputs (slots_),
    // not into RGBA. Each time it starts, it shows only frames drawn from
    // captures published from then on (PresentSlots::Start).
    void StartPresent(uint32_t width, uint32_t height, bool zero_copy) {
        {
            std::lock_guard lock(mutex_);
            if (present_) return;
            present_ = true;
            present_w_ = width;
            present_h_ = height;
            present_zero_copy_ = zero_copy;
            slots_.Start(Nanoseconds(std::chrono::steady_clock::now()));
            options_changed_ = true;
        }
        AddUser();
        WakeCaptureWaiters();
    }
    void StopPresent() {
        {
            std::lock_guard lock(mutex_);
            if (!present_) return;
            present_ = false;
            slots_.Forget();
            settle_ = nullptr;
            options_changed_ = true;
        }
        WakeCaptureWaiters();
        paint_cv_.notify_all();
        RemoveUser();
    }
    bool Presenting() {
        std::lock_guard lock(mutex_);
        return present_;
    }
    // the size the next frame for the window is drawn at, if presenting
    bool PresentDrawSize(uint32_t& width, uint32_t& height) {
        std::lock_guard lock(mutex_);
        if (!present_) return false;
        RasterOptions o;
        o.width = present_w_;
        o.height = present_h_;
        CapHeight(o);
        width = o.width;
        height = o.height;
        return true;
    }
    // the window's picture's size; the worker draws the next frame at it
    void SetPresentSize(uint32_t width, uint32_t height) {
        {
            std::lock_guard lock(mutex_);
            if (width == present_w_ && height == present_h_) return;
            present_w_ = width;
            present_h_ = height;
            options_changed_ = true;
        }
        WakeCaptureWaiters();
    }
    void SetPresentZeroCopy(bool zero_copy) {
        {
            std::lock_guard lock(mutex_);
            if (present_zero_copy_ == zero_copy) return;
            options_changed_ = true;
            present_zero_copy_ = zero_copy;
        }
        WakeCaptureWaiters();
    }
    // Direct3D 12: lets the worker learn that the GPU has finished every paint
    // so far once paints have stopped (minimized), when no paint passes on a
    // completed index: true once it has
    void SetSettle(std::function<bool()> settle) {
        std::lock_guard lock(mutex_);
        settle_ = std::move(settle);
    }
    // A paint numbered `submission`, the GPU having finished those up to
    // `completed`: the newest output to show, marked as sampled by it, its
    // frame's number (PresentSlots::Serial when it was published) and when
    // the game presented that frame; false before the first
    bool ShowNewest(uint64_t submission, uint64_t completed, int& slot, GpuOutput& out,
                    uint64_t& serial, std::chrono::steady_clock::time_point& presented) {
        {
            std::lock_guard lock(mutex_);
            slots_.Completed(completed);
            last_paint_ = std::chrono::steady_clock::now();
            paints_++;
            slot = slots_.Newest();
            if (slot >= 0) {
                slots_.Shown(slot, submission);
                out = outputs_[slot];
                serial = slot_serial_[slot];
                presented = slot_presented_[slot];
            }
        }
        // the worker may be waiting for a paint to free a slot (AcquireSlot)
        paint_cv_.notify_all();
        return slot >= 0;
    }
    // A screenshot: the next frame drawn is read back too, for Take (with
    // `for_present`), on the zero-copy path before it's published, so SDL
    // never copies an output the SDK's queue may be sampling. Returns the
    // serial Take passes.
    uint64_t RequestImage() {
        uint64_t serial;
        {
            std::lock_guard lock(mutex_);
            image_wanted_ = true;
            // drawn again even if the game has no new frame (paused)
            options_changed_ = true;
            serial = image_serial_;
        }
        WakeCaptureWaiters();
        return serial;
    }

    // the live view's numbers start over, from the next frame captured, and
    // are kept while `measure`; the window alone keeps none
    void ResetLiveStats(bool measure) {
        auto cap = LatestCapture();
        std::lock_guard lock(mutex_);
        live_measuring_ = measure;
        live_ = LiveViewStats{};
        live_base_ = live_last_ = cap ? cap->frame : 0;
        live_drew_gpu_.reset();
    }

    LiveViewStats LiveStats() {
        auto cap = LatestCapture();
        std::lock_guard lock(mutex_);
        LiveViewStats s = live_;
        s.gpu = live_drew_gpu_.value_or(gpu_);
        s.width = present_ ? present_w_ : options_.width;
        s.height = present_ ? present_h_ : options_.height;
        s.post = options_.post;
        // capturing stops with the last user, and the frame number with it
        if (users_ > 0 && cap && cap->frame > live_base_) s.captured = cap->frame - live_base_;
        else s.captured = live_last_ - live_base_;
        return s;
    }

    // copies the newest picture if it is newer than `frame`; `presented`:
    // when the game presented the frame it is of; `for_present`: only one
    // drawn for the window since presenting last started (the upload path,
    // a screenshot of it), not one left from before or drawn for the views
    bool Take(uint64_t& frame, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h,
              std::string& stats, std::chrono::steady_clock::time_point* presented = nullptr,
              bool for_present = false) {
        std::lock_guard lock(mutex_);
        stats = stats_;
        if (image_serial_ == frame || image_.empty()) return false;
        if (for_present && (!present_ || image_generation_ != slots_.Generation())) return false;
        frame = image_serial_;
        rgba = image_;
        w = image_w_;
        h = image_h_;
        if (presented) *presented = image_presented_;
        return true;
    }

 private:
    // how long paints must have stopped before the worker settles the slots
    // they sampled itself: by then each paint's commands were long submitted
    static constexpr std::chrono::milliseconds kPaintsStopped{250};
    // the longest the worker sleeps with nothing to draw before it looks
    // again; a capture, a setting changing or a user leaving wakes it sooner
    static constexpr std::chrono::milliseconds kIdleWait{100};
    // the most captures a stretch of presenting waits through for one whose
    // capture is its whole picture to start with (a screen whose frames
    // none is, if there is one, starts with what it has)
    static constexpr uint32_t kStartWait = 8;

    void Run() {
        uint64_t last_frame = 0;
        auto last_dump = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        std::vector<uint32_t> rgba;
        // as it was before the settings were last read: a capture or a
        // change since has moved it on, and the wait below doesn't sleep
        uint64_t epoch = CaptureEpoch();
        // when the window's frames are published, through the presenting
        // stretch pacer_generation
        PublishPacer pacer;
        uint64_t pacer_generation = 0;
        // the presenting stretch the last frame was drawn for; and for the
        // one starting, the captures seen while waiting for its first
        uint64_t drawn_generation = 0, start_generation = 0, start_seen = 0;
        uint32_t start_waited = 0;
        while (true) {
            RasterOptions o;
            std::string dump;
            bool changed, gpu, zero_copy, want_rgba, presenting, pace;
            // the presenting stretch this frame is drawn for (PresentSlots)
            uint64_t generation;
            {
                std::lock_guard lock(mutex_);
                if (stop_) return;
                o = options_;
                // the presenter's size wins over the window's and the live view's
                if (present_) {
                    o.width = present_w_;
                    o.height = present_h_;
                    CapHeight(o);
                }
                ScaleForPicture(o);
                o.normal_maps = REXCVAR_GET(native_view_normal_maps);
                o.filtering = REXCVAR_GET(native_view_texture_filtering);
                o.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
                // drawn frame after frame, so the trails have the post frame
                // before (the CPU's kept here, the GPU's in GpuRenderer)
                o.trails = true;
                o.post_history = &post_history_;
                // and the frames that post-process nothing show the post
                // buffer, as the game's do, not their own world
                o.post_buffer = true;
                // and the world's REFRACT_WORLD draws (the title's road) read
                // the last world frame's scene, as the game's do
                o.pre_buffer = true;
                // what each GPU draw is, for a GPU hang's DRED report
                o.gpu_labels = REXCVAR_GET(dred);
                gpu = gpu_;
                dump = dump_path_;
                zero_copy = present_ && present_zero_copy_ && gpu && dump.empty();
                // RGBA for F9's window, a screenshot, the upload path and the CPU
                want_rgba = !zero_copy || dialog_open_ || image_wanted_;
                changed = options_changed_;
                options_changed_ = false;
                presenting = present_;
                generation = slots_.Generation();
                pace = presenting && REXCVAR_GET(native_present_pacing);
            }
            // a capture published after this is a new one (the pacing's
            // wait, below)
            const uint64_t seen = CaptureEpoch();
            std::chrono::steady_clock::time_point presented;
            auto cap = LatestCapture(presented);
            bool fresh = true;
            if (cap && presenting) {
                std::lock_guard lock(mutex_);
                fresh = slots_.Fresh(Nanoseconds(presented));
            }
            // Presenting just started, and the newest capture is from before
            // (the last one before capture stopped, if no view kept it
            // going): nothing is drawn for the window until the game
            // publishes one, and the stretch's first frame is one whose
            // capture is its whole picture (StartsPicture), or else the
            // kStartWait-th. A setting changing meanwhile needs no redraw,
            // the next capture being a new frame anyway.
            if (cap && fresh && presenting && drawn_generation != generation) {
                if (start_generation != generation) {
                    start_generation = generation;
                    start_waited = 0;
                }
                if (cap->frame != start_seen) {
                    start_seen = cap->frame;
                    start_waited++;
                }
                if (!StartsPicture(*cap) && start_waited < kStartWait) fresh = false;
            }
            if (!cap || !fresh || (cap->frame == last_frame && !changed)) {
                // until the game publishes a capture, or a setting or a user
                // changes (WakeCaptureWaiters)
                epoch = WaitForCapture(epoch, kIdleWait);
                continue;
            }
            // an output no paint may still be sampling, or wait for one
            int slot = -1;
            if (zero_copy) {
                slot = AcquireSlot();
                if (slot < 0) {
                    std::lock_guard lock(mutex_);
                    options_changed_ |= changed;
                    continue;
                }
            }
            const bool new_frame = cap->frame != last_frame;
            last_frame = cap->frame;
            if (presenting) drawn_generation = generation;
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
            RasterStats rs;
            GpuOutput out;
            bool drew_gpu = false, drew_output = false;
            if (slot >= 0) {
                drew_output = GpuRenderer::Get().RenderFrameToOutput(*cap, o, slot, out, gs);
                drew_gpu = drew_output;
                // an output that failed the zero-copy checks is read back, and
                // the drawer turns to uploading from the next paint
                if (drew_output && (want_rgba || !out.d3d12_resource)) {
                    uint32_t w = 0, h = 0;
                    drew_gpu = GpuRenderer::Get().DownloadOutput(slot, rgba, w, h);
                    want_rgba = true;
                }
            } else if (gpu) {
                drew_gpu = GpuRenderer::Get().RenderFrame(*cap, o, rgba, gs);
            }
            if (!drew_gpu) {
                rs = Rasterize(*cap, o, rgba);
                want_rgba = true;
            }
            const std::string stats =
                Describe(*cap, drew_gpu ? DescribeGpu(gs) : DescribeRaster(rs));
            // a new frame for the window is published a steady delay after
            // the game presented it (PublishPacer), or sooner once the next
            // capture is there or a setting or a user changes; a redraw (new
            // options, a screenshot of a paused game) at once
            if (pace && new_frame) {
                if (generation != pacer_generation) {
                    pacer.Reset();
                    pacer_generation = generation;
                }
                const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
                const int64_t due = pacer.Due(cap->frame, Nanoseconds(presented), now);
                if (due > now) WaitForCapture(seen, std::chrono::nanoseconds(due - now));
            }
            {
                std::lock_guard lock(mutex_);
                // still the stretch it was drawn for (PresentSlots::Publish)
                const bool current = presenting && generation == slots_.Generation();
                if (slot >= 0) {
                    if (drew_output && slots_.Publish(slot, generation)) {
                        outputs_[slot] = out;
                        slot_serial_[slot] = slots_.Serial();
                        slot_presented_[slot] = presented;
                    } else {
                        slots_.Abandon(slot);
                    }
                }
                // a capture from before the numbers started over (one left
                // from the last time, or redrawn for new options) isn't counted
                if (live_measuring_ && cap->frame > live_last_) {
                    live_.skipped_busy += cap->frame - live_last_ - 1;
                    live_last_ = cap->frame;
                    live_.rendered++;
                    if (ProcKnown(*cap) && !DrawsWorld(*cap) && !cap->composed) live_.worldless++;
                    live_.ms.push_back(drew_gpu ? gs.ms : rs.ms);
                    if (drew_gpu) live_.wait_ms.push_back(gs.wait_ms);
                }
                live_drew_gpu_ = drew_gpu;
                if (want_rgba) {
                    image_ = rgba;
                    image_w_ = o.width;
                    image_h_ = o.height;
                    image_presented_ = presented;
                    image_serial_++;
                    image_generation_ = current ? generation : 0;
                    // a screenshot asked for while presenting is of a frame
                    // drawn for the window's stretch (Take's `for_present`)
                    if (current || !present_) image_wanted_ = false;
                }
                stats_ = stats;
            }
        }
    }

    // A slot for the zero-copy path's next frame, or -1 after waiting for
    // paints to finish with one (or to stop). Once paints have stopped for
    // kPaintsStopped, the GPU is asked (settle_) whether it has finished them
    // all, and then every slot they sampled is free.
    int AcquireSlot() {
        std::function<bool()> settle;
        uint64_t last_used = 0, paints = 0;
        {
            std::lock_guard lock(mutex_);
            const int slot = slots_.Acquire();
            if (slot >= 0 || stop_ || !present_) return slot;
            paints = paints_;
            if (settle_ && std::chrono::steady_clock::now() - last_paint_ >= kPaintsStopped &&
                slots_.LastUsed() > slots_.CompletedIndex()) {
                settle = settle_;
                last_used = slots_.LastUsed();
            }
        }
        if (settle && settle()) {
            std::lock_guard lock(mutex_);
            slots_.Completed(last_used);
            return -1;  // taken next time round
        }
        // Nothing else frees a slot but a paint (the GPU's completed index it
        // passes on, ShowNewest), so wait for the next one, or until paints
        // count as stopped and settling can be tried. Without a fence to
        // settle with, or with one that failed, a window that stopped
        // painting frees none until it paints again: wait for that, looking
        // every kIdleWait, rather than every few milliseconds for good.
        std::unique_lock lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto until = now + kIdleWait;
        if (settle_ && !settle) until = std::min(until, std::max(now, last_paint_ + kPaintsStopped));
        paint_cv_.wait_until(lock, until, [&] { return stop_ || !present_ || paints_ != paints; });
        return -1;
    }

    std::mutex mutex_;
    std::thread thread_;
    // the CPU's post buffer, for the trails and the frames that show it, and
    // its pre-process buffer, the worker's alone
    post::PostHistory post_history_;
    int users_ = 0;
    bool stop_ = false;
    RasterOptions options_;
    bool gpu_ = false;
    bool options_changed_ = false;
    bool dialog_open_ = false;
    std::string dump_path_;
    std::vector<uint32_t> image_;
    uint32_t image_w_ = 0, image_h_ = 0;
    uint64_t image_serial_ = 0;
    // the presenting stretch it was drawn for (PresentSlots::Generation), or
    // 0 for none
    uint64_t image_generation_ = 0;
    std::chrono::steady_clock::time_point image_presented_{};
    bool image_wanted_ = false;
    uint32_t dump_count_ = 0;
    std::string stats_;
    // the presenter's: its size and path, the outputs' bookkeeping and what
    // each holds, and when a paint last came
    bool present_ = false;
    uint32_t present_w_ = 1280, present_h_ = 720;
    bool present_zero_copy_ = false;
    PresentSlots slots_;
    GpuOutput outputs_[PresentSlots::kCount];
    // each output's frame: its number as published, and when the game
    // presented it
    uint64_t slot_serial_[PresentSlots::kCount] = {};
    std::chrono::steady_clock::time_point slot_presented_[PresentSlots::kCount] = {};
    std::chrono::steady_clock::time_point last_paint_{};
    // paints so far (ShowNewest), and the worker's wait for the next one
    uint64_t paints_ = 0;
    std::condition_variable paint_cv_;
    std::function<bool()> settle_;
    // the live view's numbers; captures numbered up to live_base_ came before
    // they started, and live_last_ is the newest one counted
    bool live_measuring_ = false;
    LiveViewStats live_;
    uint64_t live_base_ = 0, live_last_ = 0;
    std::optional<bool> live_drew_gpu_;
};
static_assert(PresentSlots::kCount == GpuRenderer::kOutputs,
              "a presenter slot is a gpu_view output");

bool g_dumping = false;
std::mutex g_live_mutex;
bool g_live = false;

}  // namespace

NativeViewDialog::NativeViewDialog(rex::ui::ImGuiDrawer* imgui_drawer, DrawerGetter drawer)
    : rex::ui::ImGuiDialog(imgui_drawer), drawer_(std::move(drawer)) {}

NativeViewDialog::~NativeViewDialog() {
    if (!visible_) return;
    Renderer::Get().SetDialogOpen(false);
    Renderer::Get().RemoveUser();
}

bool NativeViewDialog::WantsGpu() {
    // here, on the UI thread, where SDL wants its video started
    return REXCVAR_GET(native_view_backend) == "gpu" && GpuRenderer::Get().Init();
}

void NativeViewDialog::Toggle() {
    visible_ = !visible_;
    Renderer::Get().SetDialogOpen(visible_);
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
        changed |= ImGui::Checkbox("Placeholder light", &options_.legacy_light);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Skinning", &options_.skinning);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Blending", &options_.blending);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Culling", &options_.culling);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Clear depth per camera", &options_.clear_depth_per_camera);
        ImGui::SameLine();
        // RB3's depth of field, bloom and colour matrix (post_model.h)
        changed |= ImGui::Checkbox("Post-processing", &options_.post);
        ImGui::SameLine();
        // the display's gamma ramp, as the game's own picture has it (gamma_ramp.h)
        changed |= ImGui::Checkbox("Gamma ramp", &options_.gamma);
        // the native renderer on the window owns the size while it draws
        const bool presenting = Renderer::Get().Presenting();
        ImGui::BeginDisabled(presenting);
        int size = options_.width >= 1280 ? 2 : options_.width >= 960 ? 1 : 0;
        if (ImGui::Combo("Size", &size, "640x360\0960x540\01280x720\0")) {
            const uint32_t widths[] = {640, 960, 1280};
            options_.width = widths[size];
            options_.height = options_.width * 9 / 16;
            changed = true;
        }
        ImGui::EndDisabled();
        if (presenting) {
            ImGui::SameLine();
            ImGui::TextUnformatted("(renderer is native: the size follows the window)");
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

void StartLiveView(uint32_t width, uint32_t height, bool post) {
    std::lock_guard lock(g_live_mutex);
    RasterOptions o;
    o.width = width;
    o.height = height;
    o.post = post;
    Renderer::Get().SetOptions(o);
    // here, on the UI thread, where SDL wants its video started
    Renderer::Get().SetGpu(REXCVAR_GET(native_view_backend) == "gpu" &&
                           GpuRenderer::Get().Init());
    Renderer::Get().ResetLiveStats(true);
    if (!g_live) Renderer::Get().AddUser();
    g_live = true;
}

void StopLiveView() {
    std::lock_guard lock(g_live_mutex);
    if (!g_live) return;
    g_live = false;
    Renderer::Get().ResetLiveStats(false);
    Renderer::Get().RemoveUser();
}

bool LiveViewOn() {
    std::lock_guard lock(g_live_mutex);
    return g_live;
}

LiveViewStats GetLiveViewStats() { return Renderer::Get().LiveStats(); }

void StopNativeView() {
    if (g_dumping) {
        g_dumping = false;
        Renderer::Get().RemoveUser();
    }
    StopLiveView();
}

// ---------------------------------------------------------------------------
// the native renderer on the window (renderer = native)

namespace {

// The window's paints, for the harness's present_stats (GetPresentPaintStats):
// the drawer's Draw notes every one, whichever renderer, on the UI thread.
// `path` is how the native renderer's frames last reached the window.
std::mutex g_paints_mutex;
PaintRecorder g_paints;
std::chrono::steady_clock::time_point g_paints_since = std::chrono::steady_clock::now();
std::string g_present_path;

// a paint at `now`; with `native`, showing native frame `serial` (0 none) of
// `source`'s numbering, which the game presented at `presented`
void NotePaint(std::chrono::steady_clock::time_point now, bool native, int source = 0,
               uint64_t serial = 0, std::chrono::steady_clock::time_point presented = {}) {
    std::lock_guard lock(g_paints_mutex);
    g_paints.Paint(Nanoseconds(now), native, source, serial,
                   presented.time_since_epoch().count() ? Nanoseconds(presented) : 0);
}

// the paths' numberings of their frames (PaintRecorder's sources); zero-copy
// is Direct3D 12's alone
[[maybe_unused]] constexpr int kZeroCopyFrames = 0;
constexpr int kUploadedFrames = 1;

// the SDK's present_letterbox, which it reads where it paints; on if it isn't
// there to read
bool PresentLetterbox() {
    return !rex::cvar::GetFlagInfo("present_letterbox") ||
           rex::cvar::Query<bool>("present_letterbox");
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// Learns whether the GPU has finished every paint submitted so far: a fence
// of band3's own, signalled on the SDK's direct queue behind them. Only a
// signal, never a wait on that queue, which the emulated GPU submits to.
// Shared with the worker (Renderer::SetSettle), which may still be calling
// it while the drawer stops.
class PaintFence {
 public:
    static std::shared_ptr<PaintFence> Create(ID3D12Device* device, ID3D12CommandQueue* queue) {
        auto f = std::make_shared<PaintFence>();
        f->queue_ = queue;
        f->event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!f->event_ || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                     IID_PPV_ARGS(&f->fence_))))
            return nullptr;
        return f;
    }
    ~PaintFence() {
        if (event_) CloseHandle(event_);
    }

    // true once everything submitted before the call has finished, within
    // `timeout`
    bool Settle(DWORD timeout_ms) {
        std::lock_guard lock(mutex_);
        const UINT64 value = ++value_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) return false;
        if (fence_->GetCompletedValue() < value &&
            SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
            WaitForSingleObject(event_, timeout_ms);
        // the fence, not the event, which a wait that timed out leaves set
        return fence_->GetCompletedValue() >= value;
    }

 private:
    std::mutex mutex_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 value_ = 0;
};

// The zero-copy path's drawing: present.hlsl's triangle over the presenter's
// back buffer, on its own command list, sampling gpu_view's output through a
// shader-visible heap of band3's own with a descriptor per output. The
// presenter has bound its back buffer; the rest is set here, and the SDK's
// immediate drawer (ImGui, z 64) sets its own after (the N2 kill test showed
// it does).
class D3D12Present {
 public:
    bool Init(const rex::ui::d3d12::D3D12Provider& provider) {
        device_ = provider.GetDevice();
        HRESULT hr;
        {
            // b0: PresentConstants, 8 values; t0: the frame, through the
            // heap; s0: linear and clamping
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.ShaderRegister = 0;
            params[0].Constants.Num32BitValues = 8;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges = &range;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2;
            rsd.pParameters = params;
            rsd.NumStaticSamplers = 1;
            rsd.pStaticSamplers = &sampler;
            ComPtr<ID3DBlob> blob, errors;
            hr = provider.SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                                 &errors);
            if (SUCCEEDED(hr))
                hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                                  blob->GetBufferSize(), IID_PPV_ARGS(&root_));
            if (FAILED(hr)) {
                REXLOG_WARN("native present: no root signature ({:#x}) {}", uint32_t(hr),
                            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return false;
            }
        }
        using namespace shaders;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.VS = {kPresentVertexDxbc, sizeof(kPresentVertexDxbc)};
        pd.PS = {kPresentPixelDxbc, sizeof(kPresentPixelDxbc)};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rex::ui::d3d12::D3D12Presenter::kSwapChainFormat;
        pd.SampleDesc.Count = 1;
        hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline_));
        if (FAILED(hr)) {
            REXLOG_WARN("native present: no pipeline ({:#x})", uint32_t(hr));
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = GpuRenderer::kOutputs;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
        if (FAILED(hr)) {
            REXLOG_WARN("native present: no descriptor heap ({:#x})", uint32_t(hr));
            return false;
        }
        descriptor_size_ = provider.GetViewDescriptorSize();
        return true;
    }

    // output `slot`, holding `out`, into `rect` of the back buffer; black
    // around it
    void Draw(rex::ui::d3d12::D3D12UIDrawContext& ctx, const ImageRect& rect, int slot,
              const GpuOutput& out) {
        ID3D12GraphicsCommandList* cl = ctx.command_list();
        const uint32_t w = ctx.render_target_width(), h = ctx.render_target_height();
        if (!cl || !w || !h || !rect.w || !rect.h) return;
        // A new texture in the slot: its view, made over the old one. No
        // paint the GPU hasn't finished sampled the slot (the worker only
        // draws into one it's done with, PresentSlots), so neither the old
        // view nor the old texture, which it holds until now, is in use.
        View& view = views_[slot];
        if (view.generation != out.generation || !view.resource) {
            view.resource = static_cast<ID3D12Resource*>(out.d3d12_resource);
            view.generation = out.generation;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(slot) * descriptor_size_;
            device_->CreateShaderResourceView(view.resource.Get(), &sd, cpu);
        }
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * descriptor_size_;
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(root_.Get());
        cl->SetPipelineState(pipeline_.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
        cl->RSSetViewports(1, &viewport);
        const D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
        cl->RSSetScissorRects(1, &scissor);
        // present.hlsl's PresentConstants
        const float constants[8] = {float(rect.x),
                                    float(rect.y),
                                    float(rect.w),
                                    float(rect.h),
                                    float(out.width),
                                    float(out.height),
                                    out.width == rect.w && out.height == rect.h ? 1.0f : 0.0f,
                                    0.0f};
        cl->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
        cl->SetGraphicsRootDescriptorTable(1, gpu);
        cl->DrawInstanced(3, 1, 0, 0);
    }

 private:
    ID3D12Device* device_ = nullptr;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptor_size_ = 0;
    // each output's texture as its view was made, held while paints may
    // sample it (SDL lets its own reference go when it makes the slot again)
    struct View {
        ComPtr<ID3D12Resource> resource;
        uint64_t generation = 0;
    };
    View views_[GpuRenderer::kOutputs];
};
#endif

// on the UI thread: the drawer follows renderer again (the emulated GPU's
// picture has turned fresh), if it's still there
void FollowRendererLater();

// The UI drawer: always on the presenter at z 0, under ImGui (64), drawing
// nothing while renderer is emulated. While native it owns the worker's size
// (the window's picture's, LetterboxRect) and draws the newest frame each
// paint, repeating it until a newer one comes: zero-copy on Direct3D 12, or
// else uploaded through the SDK's immediate drawer.
class NativePresentDrawer : public rex::ui::UIDrawer {
 public:
    using ImmediateGetter = std::function<rex::ui::ImmediateDrawer*()>;
    NativePresentDrawer(rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
                        rex::ui::Window* window, ImmediateGetter immediate)
        : presenter_(presenter), provider_(provider), window_(window),
          immediate_(std::move(immediate)) {}

    void Start() {
#ifdef _WIN32
        // The only graphics provider band3's SDK has on Windows is Direct3D
        // 12's (rexruntime.dll exports no Vulkan), so it and the presenter
        // are D3D12Provider and D3D12Presenter.
        if (provider_) {
            const auto& d3d12 = static_cast<const rex::ui::d3d12::D3D12Provider&>(*provider_);
            // Microsoft's software rasterizer (WARP, and the Basic Render
            // Driver Windows falls back to without a GPU driver): SDL_gpu
            // would get the SDK's own device there too, and band3's frames
            // and the emulated GPU's running on it together fault inside
            // WARP, killing band3, on either path. On separate devices they
            // don't; on hardware sharing is fine.
            if (d3d12.GetAdapterVendorID() == rex::ui::GraphicsProvider::GpuVendorID::kMicrosoft)
                GpuRenderer::Get().RefuseDevice(
                    "the SDK's GPU is Microsoft's software rasterizer (WARP), whose Direct3D 12 "
                    "device band3's GPU drawing would share with the emulated GPU, and that "
                    "crashes there");
            GpuRenderer::Get().SetPresentDevice(d3d12.GetDevice());
            // the crash trace's DRED report if the device is removed (a GPU
            // hang), whichever renderer is on; here, as the one place band3
            // has the SDK's device from the start
            crash_trace::WatchD3D12Device(d3d12.GetDevice(), d3d12.GetDirectQueue());
            // and, stopped at an indexed draw, which of the native renderer's
            // it was (RasterOptions::gpu_labels)
            crash_trace::SetIndexedDrawNamer([](uint32_t before, uint32_t total) {
                return GpuRenderer::Get().DescribeIndexedDraw(before, total);
            });
            auto present = std::make_unique<D3D12Present>();
            if (present->Init(d3d12)) d3d12_ = std::move(present);
            else d3d12_why_ = "its Direct3D 12 pipeline couldn't be made (see the log)";
            fence_ = PaintFence::Create(d3d12.GetDevice(), d3d12.GetDirectQueue());
        } else {
            d3d12_why_ = "the SDK has no graphics provider";
        }
#else
        (void)provider_;  // the immediate drawer is the provider's already
#endif
        presenter_->AddUIDrawerFromUIThread(this, 0);
        // F8 back to emulated waits for the emulated GPU's picture; told on
        // the game's thread, followed on the UI thread
        if (window_) {
            rex::ui::WindowedAppContext* app = &window_->app_context();
            SetEmulatedFreshCallback(
                [app] { app->CallInUIThreadDeferred([] { FollowRendererLater(); }); });
        }
        // F4, F8 and the harness's `set` change it on the UI thread
        rex::cvar::RegisterChangeCallback("renderer", [this](std::string_view, std::string_view v) {
            Follow(v == "native");
        });
        Follow(REXCVAR_GET(renderer) == "native");
    }

    void Stop() {
        SetEmulatedFreshCallback(nullptr);
        rex::cvar::UnregisterChangeCallbacks("renderer");
        presenter_->RemoveUIDrawerFromUIThread(this);
        Follow(false, true);
#ifdef _WIN32
        // the GPU done with every paint that sampled an output before the
        // textures they hold are let go (no paint runs now: this is the UI
        // thread)
        if (fence_ && drew_) fence_->Settle(2000);
        d3d12_.reset();
        crash_trace::WatchD3D12Device(nullptr, nullptr);
        crash_trace::SetIndexedDrawNamer(nullptr);
#endif
        for (auto& t : textures_) t.reset();
    }

    // renderer, as it changes: the worker starts when it turns native, on the
    // UI thread where the GPU device starts, and is let go when it turns
    // emulated, once the emulated GPU's picture is the game's again: while
    // native it skipped the game's draws (gpu_skip.h), so it keeps drawing
    // the window until the emulated GPU has swapped whole frames enough
    // (SkipLatch::Fresh: two, and with even/odd rendering a post frame after
    // a whole world frame; gpu_skip.cpp calls back then), or kMaxDrain has
    // gone by
    // (`at_once`: at shutdown, without waiting)
    void Follow(bool native, bool at_once = false) {
        // native again while draining: it just goes on
        if (native) draining_ = false;
        if (native == active_) return;
        if (!native) {
            const auto now = std::chrono::steady_clock::now();
            if (!at_once && !EmulatedPictureFresh()) {
                if (!draining_) {
                    draining_ = true;
                    drain_start_ = now;
                    REXLOG_INFO("native present: drawing on until the emulated GPU has drawn "
                                "whole frames again");
                }
                if (now - drain_start_ < kMaxDrain) return;
                REXLOG_WARN("native present: the emulated GPU drew no whole frames in {} ms; "
                            "its picture may be stale for a moment",
                            kMaxDrain.count());
            }
            const double waited =
                draining_ ? std::chrono::duration<double, std::milli>(now - drain_start_).count()
                          : 0.0;
            draining_ = false;
            active_ = false;
            Renderer::Get().StopPresent();
            REXLOG_INFO("native present: off, the emulated GPU's picture shows (after {:.0f} ms)",
                        waited);
            return;
        }
        active_ = true;
        // every pipeline made now, here on the UI thread, not by the
        // worker's first frame while the window waits for it
        if (UpdateGpu()) GpuRenderer::Get().Prewarm(uint32_t(REXCVAR_GET(native_view_msaa)));
        const bool zero_copy = ChoosePath();
        // before the first paint, the window's size (minimized, a paint may
        // not come for a while)
        ImageRect rect = rect_;
        if (!rect.w || !rect.h) {
            const uint32_t w = window_ ? window_->GetActualPhysicalWidth() : 0;
            const uint32_t h = window_ ? window_->GetActualPhysicalHeight() : 0;
            rect = w && h ? LetterboxRect(w, h, PresentLetterbox()) : ImageRect{0, 0, 1280, 720};
        }
        Renderer::Get().StartPresent(rect.w, rect.h, zero_copy);
        // the upload path's last picture is from before: none until this
        // stretch's first
        upload_shown_ = false;
#ifdef _WIN32
        if (fence_) {
            std::shared_ptr<PaintFence> fence = fence_;
            Renderer::Get().SetSettle([fence] { return fence->Settle(50); });
        }
#endif
        REXLOG_INFO("native present: on, drawing at {}x{}", rect.w, rect.h);
    }

    void Draw(rex::ui::UIDrawContext& context) override {
        // every paint is timed, whichever renderer it shows (present_stats)
        const auto now = std::chrono::steady_clock::now();
        // read every paint, as well as followed as it changes
        Follow(REXCVAR_GET(renderer) == "native");
        if (!active_) {
            NotePaint(now, false);
            return;
        }
        const uint32_t tw = context.render_target_width(), th = context.render_target_height();
        if (!tw || !th) {
            NotePaint(now, true);
            return;
        }
        rect_ = LetterboxRect(tw, th, PresentLetterbox());
        Renderer::Get().SetPresentSize(rect_.w, rect_.h);
        UpdateGpu();
#ifdef _WIN32
        if (ChoosePath()) {
            auto& ctx = static_cast<rex::ui::d3d12::D3D12UIDrawContext&>(context);
            int slot = -1;
            GpuOutput out;
            uint64_t serial = 0;
            std::chrono::steady_clock::time_point presented;
            if (Renderer::Get().ShowNewest(ctx.submission_index_current(),
                                           ctx.submission_index_completed(), slot, out, serial,
                                           presented) &&
                out.d3d12_resource) {
                d3d12_->Draw(ctx, rect_, slot, out);
                drew_ = true;
                NotePaint(now, true, kZeroCopyFrames, serial, presented);
            } else {
                NotePaint(now, true);
            }
            return;
        }
#else
        ChoosePath();
#endif
        DrawUploaded(context, now);
    }

 private:
    // native_view_backend, which F4 can change: on the GPU when its device
    // starts (here, on the UI thread), else on the CPU; true on the GPU
    bool UpdateGpu() {
        const bool gpu = REXCVAR_GET(native_view_backend) == "gpu" && GpuRenderer::Get().Init();
        Renderer::Get().SetGpu(gpu);
        return gpu;
    }

    // zero-copy, or uploading and why; logged when it changes
    bool ChoosePath() {
        std::string why;
        bool zero_copy = false;
#ifdef _WIN32
        if (!d3d12_) why = d3d12_why_;
        else if (!REXCVAR_GET(native_present_zero_copy)) why = "native_present_zero_copy is off";
        else if (REXCVAR_GET(native_view_backend) != "gpu") why = "native_view_backend is cpu";
        else zero_copy = GpuRenderer::Get().CheckZeroCopy(why);
#else
        why = "zero-copy presentation is Direct3D 12 only";
#endif
        if (!path_logged_ || zero_copy != zero_copy_ || why != why_) {
            if (zero_copy) REXLOG_INFO("native present: zero-copy");
            else REXLOG_INFO("native present: uploading each frame ({})", why);
            path_logged_ = true;
        }
        zero_copy_ = zero_copy;
        why_ = why;
        Renderer::Get().SetPresentZeroCopy(zero_copy);
        {
            std::lock_guard lock(g_paints_mutex);
            g_present_path = zero_copy ? "zero-copy" : "upload";
        }
        return zero_copy;
    }

    // the upload path: the worker's newest RGBA as an immediate texture (two
    // in turn, so the one a paint in flight reads isn't the one let go),
    // drawn through the SDK's immediate drawer in rect_, black around it
    void DrawUploaded(rex::ui::UIDrawContext& context, std::chrono::steady_clock::time_point now) {
        rex::ui::ImmediateDrawer* drawer = immediate_ ? immediate_() : nullptr;
        if (!drawer) {
            NotePaint(now, true);
            return;
        }
        uint32_t w = 0, h = 0;
        std::string stats;
        if (Renderer::Get().Take(upload_frame_, upload_, w, h, stats, &upload_presented_, true) &&
            w && h) {
            upload_shown_ = true;
            current_ ^= 1;
            // the immediate drawer's textures are made from data only, so a
            // new one each frame
            textures_[current_] =
                drawer->CreateTexture(w, h, rex::ui::ImmediateTextureFilter::kLinear, false,
                                      reinterpret_cast<const uint8_t*>(upload_.data()));
        }
        rex::ui::ImmediateTexture* texture = upload_shown_ ? textures_[current_].get() : nullptr;
        if (!texture) {
            NotePaint(now, true);
            return;
        }
        NotePaint(now, true, kUploadedFrames, upload_frame_, upload_presented_);
        const float tw = float(context.render_target_width());
        const float th = float(context.render_target_height());
        const float x0 = float(rect_.x), y0 = float(rect_.y);
        const float x1 = x0 + float(rect_.w), y1 = y0 + float(rect_.h);
        // ImGui's colours: ABGR
        constexpr uint32_t kBlack = 0xff000000u, kWhite = 0xffffffffu;
        const rex::ui::ImmediateVertex v[12] = {
            // black over all of it, the bars
            {0, 0, 0, 0, kBlack}, {tw, 0, 0, 0, kBlack}, {tw, th, 0, 0, kBlack},
            {0, 0, 0, 0, kBlack}, {tw, th, 0, 0, kBlack}, {0, th, 0, 0, kBlack},
            // the picture
            {x0, y0, 0, 0, kWhite}, {x1, y0, 1, 0, kWhite}, {x1, y1, 1, 1, kWhite},
            {x0, y0, 0, 0, kWhite}, {x1, y1, 1, 1, kWhite}, {x0, y1, 0, 1, kWhite},
        };
        drawer->Begin(context, tw, th);
        rex::ui::ImmediateDrawBatch batch;
        batch.vertices = v;
        batch.vertex_count = 12;
        drawer->BeginDrawBatch(batch);
        rex::ui::ImmediateDraw draw;
        draw.primitive_type = rex::ui::ImmediatePrimitiveType::kTriangles;
        draw.count = 6;
        drawer->Draw(draw);
        draw.base_vertex = 6;
        draw.texture = texture;
        drawer->Draw(draw);
        drawer->EndDrawBatch();
        drawer->End();
    }

    rex::ui::Presenter* presenter_;
    rex::ui::GraphicsProvider* provider_;
    rex::ui::Window* window_;
    ImmediateGetter immediate_;
    bool active_ = false;
    // renderer turned emulated, and the window is drawn on until the emulated
    // GPU's picture is fresh (Follow), since drain_start_
    bool draining_ = false;
    std::chrono::steady_clock::time_point drain_start_;
    static constexpr std::chrono::milliseconds kMaxDrain{3000};
    // the picture's place in the back buffer at the last paint
    ImageRect rect_;
    bool path_logged_ = false;
    bool zero_copy_ = false;
    std::string why_;
    // the upload path's
    std::unique_ptr<rex::ui::ImmediateTexture> textures_[2];
    int current_ = 0;
    uint64_t upload_frame_ = 0;
    // a picture taken since native last started
    bool upload_shown_ = false;
    std::chrono::steady_clock::time_point upload_presented_{};
    std::vector<uint32_t> upload_;
#ifdef _WIN32
    std::unique_ptr<D3D12Present> d3d12_;
    std::string d3d12_why_;
    std::shared_ptr<PaintFence> fence_;
    bool drew_ = false;  // a paint sampled an output
#endif
};

std::unique_ptr<NativePresentDrawer> g_present;

void FollowRendererLater() {
    if (g_present) g_present->Follow(REXCVAR_GET(renderer) == "native");
}

}  // namespace

void StartNativePresent(rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
                        rex::ui::Window* window,
                        std::function<rex::ui::ImmediateDrawer*()> immediate_drawer) {
    if (g_present || !presenter) return;
    g_present = std::make_unique<NativePresentDrawer>(presenter, provider, window,
                                                      std::move(immediate_drawer));
    g_present->Start();
}

void StopNativePresent() {
    if (!g_present) return;
    g_present->Stop();
    g_present.reset();
}

bool NativePresenting() { return Renderer::Get().Presenting(); }

PresentPaintStats GetPresentPaintStats(bool reset) {
    const bool presenting = Renderer::Get().Presenting();
    PresentPaintStats out;
    std::lock_guard lock(g_paints_mutex);
    out.log = g_paints.Log();
    out.since = g_paints_since;
    if (presenting) out.path = g_present_path;
    if (reset) {
        g_paints.Reset();
        g_paints_since = std::chrono::steady_clock::now();
    }
    return out;
}

bool NativePresentDrawSize(uint32_t& width, uint32_t& height) {
    return Renderer::Get().PresentDrawSize(width, height);
}

void ScaleForPicture(RasterOptions& o) {
    // the screen's passes (the spotlights' haze, the soft particles) keep
    // their share of a picture bigger than the game's (soft_raster.h's
    // PassTargetSize)
    o.target_scale = REXCVAR_GET(native_view_target_scale)
                         ? float(o.height) / float(post::kGameHeight)
                         : 1.0f;
    o.shadow_scale = float(REXCVAR_GET(native_view_shadow_scale));
}

std::string NativePresentedPicture(std::vector<uint32_t>& rgba, uint32_t& width,
                                   uint32_t& height, std::chrono::milliseconds wait) {
    if (!Renderer::Get().Presenting())
        return "the native renderer isn't drawing the window (renderer is emulated)";
    const auto deadline = std::chrono::steady_clock::now() + wait;
    uint64_t frame = Renderer::Get().RequestImage();
    std::string stats;
    while (!Renderer::Get().Take(frame, rgba, width, height, stats, nullptr, true)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return "the native renderer hasn't drawn a frame in " +
                   std::to_string(wait.count()) + " ms";
        if (!Renderer::Get().Presenting()) return "the native renderer stopped (renderer is emulated)";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return {};
}

}  // namespace band3::render
