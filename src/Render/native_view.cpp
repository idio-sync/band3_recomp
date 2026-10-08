#include "src/Render/native_view.h"

#include "src/Hooks/aspect.h"
#include "src/Hooks/aspect_model.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Render/camera_cut.h"
#include "src/Render/capture_file.h"
#include "src/Render/deferred_decode.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/gpu_view.h"
#include "src/Render/png_writer.h"
#include "src/Render/post_model.h"
#include "src/Render/present_model.h"
#include "src/Render/renderer_switch.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/crash_trace.h"
#include "src/settings.h"
#include "src/stall_watch.h"

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

// `output`: RenderFrameToOutput's stats, where ms ends at submit
std::string DescribeGpu(const GpuStats& gs, bool output) {
    const char* format =
        output ? "gpu: %u draws (%u texture passes, %u not drawn yet), %u uploads, %.1f ms to "
                 "submit, then %.1f ms waiting for the GPU; %u sampled a render target nothing "
                 "drew\n"
               : "gpu: %u draws (%u texture passes, %u not drawn yet), %u uploads, %.1f ms "
                 "(%.1f ms after submit); %u sampled a render target nothing drew\n";
    char buf[240];
    std::snprintf(buf, sizeof(buf), format, gs.draws, gs.passes, gs.skipped, gs.uploads, gs.ms,
                  gs.wait_ms, gs.rt_missing);
    return buf;
}

int64_t Nanoseconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// for present_stats' publish latency; defined with the paints below
void NotePublished(std::chrono::steady_clock::time_point presented);

void AddGpu(GpuStats& sum, const GpuStats& g) {
    sum.draws += g.draws;
    sum.skipped += g.skipped;
    sum.uploads += g.uploads;
    sum.passes += g.passes;
    sum.rt_missing += g.rt_missing;
    sum.ms += g.ms;
    sum.wait_ms += g.wait_ms;
    sum.decode_ms += g.decode_ms;
    sum.pre_ms += g.pre_ms;
    sum.plan_ms += g.plan_ms;
    sum.upload_ms += g.upload_ms;
    sum.record_ms += g.record_ms;
    sum.submit_ms += g.submit_ms;
    sum.evict_ms += g.evict_ms;
    sum.pre_passes += g.pre_passes;
    sum.submits += g.submits;
    sum.plan_setup_ms += g.plan_setup_ms;
    sum.plan_walk_ms += g.plan_walk_ms;
    sum.plan_arena_ms += g.plan_arena_ms;
    sum.plan_reserve_ms += g.plan_reserve_ms;
    sum.post_plan_ms += g.post_plan_ms;
    sum.targets_made += g.targets_made;
    sum.targets_new += g.targets_new;
    sum.targets_resized += g.targets_resized;
    sum.targets_returning += g.targets_returning;
    sum.targets_ms += g.targets_ms;
    sum.textures_first += g.textures_first;
    sum.meshes_first += g.meshes_first;
    sum.arrays_grown += g.arrays_grown;
    sum.arrays_ms += g.arrays_ms;
    sum.arrays_mb += g.arrays_mb;
    sum.arena_new_mb += g.arena_new_mb;
    sum.reserve_grew += g.reserve_grew;
    sum.world_draws += g.world_draws;
    sum.ahead_ms += g.ahead_ms;
    sum.ahead_used += g.ahead_used;
    sum.ahead_fallback_passes += g.ahead_fallback_passes;
    sum.ahead_gated += g.ahead_gated;
    sum.pool_meshes += g.pool_meshes;
    sum.arena_moved += g.arena_moved;
    sum.arena_sent += g.arena_sent;
    sum.arena_copied += g.arena_copied;
    sum.textures_pressured += g.textures_pressured;
    sum.meshes_pressured += g.meshes_pressured;
    sum.mesh_bytes += g.mesh_bytes;
    sum.textures_sent += g.textures_sent;
    sum.texture_bytes += g.texture_bytes;
    sum.bone_bytes += g.bone_bytes;
    sum.pipelines_made += g.pipelines_made;
    sum.buffers_made += g.buffers_made;
    sum.textures_made += g.textures_made;
    sum.evicted_meshes += g.evicted_meshes;
    sum.evicted_textures += g.evicted_textures;
    sum.evicted_rts += g.evicted_rts;
    sum.rts_released += g.rts_released;
    sum.resident_meshes += g.resident_meshes;
    sum.resident_textures += g.resident_textures;
    sum.resident_rts += g.resident_rts;
    sum.meshes_by_time += g.meshes_by_time;
    sum.textures_by_time += g.textures_by_time;
    sum.texture_array_mb += g.texture_array_mb;
    sum.arena_mb += g.arena_mb;
    sum.rts_mb += g.rts_mb;
    // only frames with gpu_timed have these
    for (int p = 0; p < gpu_timing::kParts; p++) sum.gpu_ms[p] += g.gpu_ms[p];
    sum.gpu_total_ms += g.gpu_total_ms;
    sum.gpu_marks_dropped += g.gpu_marks_dropped;
    sum.gpu_bad_spans += g.gpu_bad_spans;
}

// per-field max of plan_ms and its parts
void MostPlan(GpuStats& most, const GpuStats& g) {
    most.plan_ms = std::max(most.plan_ms, g.plan_ms);
    most.plan_setup_ms = std::max(most.plan_setup_ms, g.plan_setup_ms);
    most.plan_walk_ms = std::max(most.plan_walk_ms, g.plan_walk_ms);
    most.targets_ms = std::max(most.targets_ms, g.targets_ms);
    most.arrays_ms = std::max(most.arrays_ms, g.arrays_ms);
    most.plan_arena_ms = std::max(most.plan_arena_ms, g.plan_arena_ms);
    most.plan_reserve_ms = std::max(most.plan_reserve_ms, g.plan_reserve_ms);
    most.post_plan_ms = std::max(most.post_plan_ms, g.post_plan_ms);
}

void AddCost(FrameCapture::Cost& sum, const FrameCapture::Cost& c) {
    for (int i = 0; i < FrameCapture::Cost::kHooks; i++) sum.hook_ns[i] += c.hook_ns[i];
    sum.draws += c.draws;
    sum.new_shades += c.new_shades;
    sum.allocs += c.allocs;
    sum.bones += c.bones;
    sum.geom_copy_bytes += c.geom_copy_bytes;
    sum.tex_copy_bytes += c.tex_copy_bytes;
    sum.tex_decode_bytes += c.tex_decode_bytes;
    sum.game_ns += c.game_ns;
}

// native_slow_frame_ms's plan parts and camera, for DescribeSlow
std::string DescribePlan(const GpuStats& gs, const CameraCuts::Step& cam) {
    std::string grew;
    auto grown = [&](const char* name, const uint32_t (&kb)[2]) {
        if (kb[0] == kb[1]) return;
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %u->%u KB", grew.empty() ? "" : ", ", name, kb[0], kb[1]);
        grew += b;
    };
    grown("pool vertices", gs.pool_verts_kb);
    grown("pool indices", gs.pool_indices_kb);
    grown("bones", gs.bones_kb);
    if (grew.empty()) grew = "nothing grew";
    char arena[128] = "";
    if (gs.arena_rebuilt)
        std::snprintf(arena, sizeof(arena),
                      " (rebuilt, %.1f MB, %u meshes kept copied on the GPU, %u let go for room)",
                      gs.arena_new_mb, gs.arena_copied, gs.meshes_pressured);
    char buf[720];
    std::snprintf(buf, sizeof(buf),
                  "; plan: setup %.1f, walk %.1f (%u targets made in %.1f ms, %u returning, "
                  "%u resized; %u textures and %u meshes drawn for the first time; %u texture "
                  "arrays grown in %.1f ms, %.1f MB), arena %.1f%s, reserve %.1f (%s) ms; "
                  "record's post plan %.1f ms",
                  gs.plan_setup_ms, gs.plan_walk_ms, gs.targets_made, gs.targets_ms,
                  gs.targets_returning, gs.targets_resized, gs.textures_first, gs.meshes_first,
                  gs.arrays_grown,
                  gs.arrays_ms, gs.arrays_mb, gs.plan_arena_ms, arena, gs.plan_reserve_ms,
                  grew.c_str(), gs.post_plan_ms);
    std::string s = buf;
    if (!cam.world_frame) {
        s += "; camera: none in its world";
    } else if (!cam.new_world) {
        std::snprintf(buf, sizeof(buf), "; camera: game frame %llu's world, drawn before",
                      static_cast<unsigned long long>(cam.world_frame));
        s += buf;
    } else if (cam.jump >= 0 || cam.turn >= 0) {
        // -1 for whichever can't be told
        std::snprintf(buf, sizeof(buf),
                      "; camera: moved %.2f of the screen and turned %.1f degrees in %llu game "
                      "frames%s",
                      cam.jump, cam.turn, static_cast<unsigned long long>(cam.gap),
                      cam.cut ? ", a cut" : "");
        s += buf;
    } else {
        std::snprintf(buf, sizeof(buf), "; camera: not compared (%llu game frames since the last)",
                      static_cast<unsigned long long>(cam.gap));
        s += buf;
    }
    if (cam.vel_valid) {
        std::snprintf(buf, sizeof(buf), ", vel_frame %u%s", cam.vel_frame,
                      cam.confirmed ? " (reset, confirming the cut)"
                      : cam.vel_reset ? " (reset)"
                                      : "");
        s += buf;
    }
    if (cam.since_cut >= 0) {
        std::snprintf(buf, sizeof(buf), "; %lld game frames since the last cut",
                      static_cast<long long>(cam.since_cut));
        s += buf;
    } else {
        s += "; no cut seen yet";
    }
    return s;
}

// native_gpu_timestamps parts for DescribeSlow, largest first, under 0.05 ms
// left out; "" without timings
std::string DescribeGpuTimes(const GpuStats& gs) {
    if (!gs.gpu_timed) return "";
    int order[gpu_timing::kParts];
    for (int p = 0; p < gpu_timing::kParts; p++) order[p] = p;
    std::stable_sort(std::begin(order), std::end(order),
                     [&](int a, int b) { return gs.gpu_ms[a] > gs.gpu_ms[b]; });
    char buf[96];
    std::snprintf(buf, sizeof(buf), "; GPU %.1f ms busy:", gs.gpu_total_ms);
    std::string s = buf;
    bool first = true;
    for (int p : order) {
        if (gs.gpu_ms[p] < 0.05) break;
        std::snprintf(buf, sizeof(buf), "%s %s %.1f", first ? "" : ",",
                      gpu_timing::PartName(uint8_t(p)), gs.gpu_ms[p]);
        s += buf;
        first = false;
    }
    if (gs.gpu_marks_dropped || gs.gpu_bad_spans) {
        std::snprintf(buf, sizeof(buf), " (%u timestamps dropped, %u spans left out)",
                      gs.gpu_marks_dropped, gs.gpu_bad_spans);
        s += buf;
    }
    return s;
}

// native_slow_frame_ms's line; `skipped`: captures never drawn before it
std::string DescribeSlow(const FrameCapture& fc, const GpuStats& gs, uint64_t skipped,
                         const CameraCuts::Step& cam) {
    uint64_t hooks_ns = 0;
    for (uint64_t ns : fc.cost.hook_ns) hooks_ns += ns;
    const FrameKind kind = KindOf(fc);
    char world[64] = "";
    if (fc.composed)
        std::snprintf(world, sizeof(world), ", world from game frame %llu",
                      static_cast<unsigned long long>(fc.world_frame));
    char buf[2048];
    std::snprintf(
        buf, sizeof(buf),
        "native renderer: slow frame %llu (game frame %llu, %s, proc_cmds %u%s): %.1f ms, "
        "%.1f of it waiting for the GPU; decode %.1f, pre %.1f (%u passes), plan %.1f, upload "
        "%.1f, record %.1f, submit %.1f (%u command buffers), evict %.1f ms; %u draws (%u of the world%s), %u "
        "texture passes; "
        "sent %u meshes into the pool and %u into the arena (%.2f MB), %u textures (%.2f MB), "
        "%u KB of bones; moved %u meshes to the arena%s; made %u pipelines, %u buffers, %u "
        "textures; let go of %u meshes, %u textures, %u targets' pictures after, and released "
        "%u targets (%.1f MB of targets kept); kept %u meshes and %u textures by the clock, "
        "let %u textures go for room; its capture cost the game's thread %.2f ms "
        "(%u draws, %u new shades, %u allocations, %u bones, %llu KB of geometry and %llu KB "
        "of textures copied to decode later, %llu KB of textures decoded) in a %.1f ms game "
        "frame; %llu captures skipped before it",
        static_cast<unsigned long long>(fc.frame), static_cast<unsigned long long>(fc.game_frame),
        FrameKindName(kind), fc.proc_cmds, world, gs.ms, gs.wait_ms, gs.decode_ms, gs.pre_ms,
        gs.pre_passes,
        gs.plan_ms, gs.upload_ms, gs.record_ms, gs.submit_ms, gs.submits, gs.evict_ms, gs.draws,
        gs.world_draws, gs.shows_kept ? ", the kept post buffer shown" : "", gs.passes,
        gs.pool_meshes, gs.arena_sent, gs.mesh_bytes / 1048576.0, gs.textures_sent,
        gs.texture_bytes / 1048576.0, uint32_t(gs.bone_bytes >> 10), gs.arena_moved,
        gs.arena_rebuilt ? " (rebuilt it)" : "", gs.pipelines_made, gs.buffers_made,
        gs.textures_made, gs.evicted_meshes, gs.evicted_textures, gs.evicted_rts,
        gs.rts_released, gs.rts_mb, gs.meshes_by_time, gs.textures_by_time,
        gs.textures_pressured, hooks_ns / 1e6, fc.cost.draws, fc.cost.new_shades,
        fc.cost.allocs, fc.cost.bones,
        static_cast<unsigned long long>(fc.cost.geom_copy_bytes >> 10),
        static_cast<unsigned long long>(fc.cost.tex_copy_bytes >> 10),
        static_cast<unsigned long long>(fc.cost.tex_decode_bytes >> 10), fc.cost.game_ns / 1e6,
        static_cast<unsigned long long>(skipped));
    return buf + DescribePlan(gs, cam) + DescribeGpuTimes(gs);
}

// ---------------------------------------------------------------------------
// the worker that rasterizes the newest capture

// native_max_height: taller pictures are drawn at it, keeping aspect, and the
// presenter scales up (present.hlsl filters)
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

    // F9's window needs RGBA, which zero-copy frames otherwise never have
    void SetDialogOpen(bool open) {
        std::lock_guard lock(mutex_);
        dialog_open_ = open;
    }

    // While presenting, its size overrides the window's and live view's, and
    // zero-copy frames go into slots_ rather than RGBA. Each start shows only
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
    // D3D12: tells the worker, once paints have stopped (minimized) and none
    // pass on a completed index, whether the GPU finished every paint so far
    void SetSettle(std::function<bool()> settle) {
        std::lock_guard lock(mutex_);
        settle_ = std::move(settle);
    }
    // Renderer native, minimized: draw nothing but screenshots, keep outputs.
    // Restored: keep showing the last frame (not a new stretch, which would
    // show black) until one from a newer capture (PresentSlots::Resume),
    // which must be a whole picture since the kept post buffer is stale.
    void SetPaused(bool paused) {
        const uint64_t frame = LatestCaptureFrame();
        const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
        double last_ms = 0;
        uint64_t last_captures = 0;
        {
            std::lock_guard lock(mutex_);
            if (paused) {
                if (!pause_.Pause(now, frame)) return;
            } else {
                if (!pause_.Resume(now, frame)) return;
                if (present_) slots_.Resume(now);
                resumes_++;
                // captures meanwhile don't count as skipped_busy
                live_last_ = std::max(live_last_, frame);
                options_changed_ = true;
                last_ms = pause_.LastMs();
                last_captures = pause_.LastCaptures();
            }
        }
        if (paused) {
            REXLOG_INFO("native renderer: the window is minimized; drawing nothing until it's "
                        "restored");
        } else {
            REXLOG_INFO("native renderer: the window is restored; drawing again, after {:.0f} ms "
                        "paused and {} captures not drawn",
                        last_ms, last_captures);
        }
        WakeCaptureWaiters();
        paint_cv_.notify_all();
    }
    // called on the worker per frame published for the window; null to stop
    void SetPublished(std::function<void()> published) {
        std::lock_guard lock(mutex_);
        published_ = published ? std::make_shared<const std::function<void()>>(std::move(published))
                               : nullptr;
    }
    // For paint `submission` (GPU done through `completed`): the newest output,
    // marked sampled by it, with its serial and Present time; false before
    // the first
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
        // the worker may be waiting for a free slot (AcquireSlot)
        paint_cv_.notify_all();
        return slot >= 0;
    }
    // A screenshot: the next frame is read back for Take, on the zero-copy
    // path before publishing so SDL never copies an output the SDK's queue
    // may be sampling. Returns the serial for Take.
    uint64_t RequestImage() {
        uint64_t serial;
        {
            std::lock_guard lock(mutex_);
            image_wanted_ = true;
            // redraw even without a new game frame (paused)
            options_changed_ = true;
            serial = image_serial_;
        }
        WakeCaptureWaiters();
        // a paused worker waits on this instead
        paint_cv_.notify_all();
        return serial;
    }

    // restarts live stats from the next capture, kept while `measure`
    void ResetLiveStats(bool measure) {
        const uint64_t frame = LatestCaptureFrame();
        std::lock_guard lock(mutex_);
        live_measuring_ = measure;
        live_ = LiveViewStats{};
        live_base_ = live_last_ = frame;
        live_last_kind_ = -1;
        live_drew_gpu_.reset();
        pause_.Restart(Nanoseconds(std::chrono::steady_clock::now()), live_base_);
    }

    LiveViewStats LiveStats() {
        // number only; decoding is the worker's
        const uint64_t frame = LatestCaptureFrame();
        std::lock_guard lock(mutex_);
        LiveViewStats s = live_;
        s.gpu = live_drew_gpu_.value_or(gpu_);
        s.width = present_ ? present_w_ : options_.width;
        s.height = present_ ? present_h_ : options_.height;
        s.post = options_.post;
        // capturing, and the frame number, stop with the last user
        if (users_ > 0 && frame > live_base_) s.captured = frame - live_base_;
        else s.captured = live_last_ - live_base_;
        s.paused = pause_.Paused();
        s.paused_ms = pause_.Ms(Nanoseconds(std::chrono::steady_clock::now()));
        s.paused_captures = pause_.Captures(frame);
        return s;
    }

    // copies the newest picture if newer than `frame`; `for_present`: only
    // one drawn for the window since presenting last started
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
    // paints stopped this long: the worker settles their slots itself, as
    // their commands were long submitted
    static constexpr std::chrono::milliseconds kPaintsStopped{250};
    // idle poll; captures, settings and users leaving wake it sooner
    static constexpr std::chrono::milliseconds kIdleWait{100};
    // captures a presenting stretch waits through for a whole picture to
    // start with
    static constexpr uint32_t kStartWait = 8;
    // Fence polling, as SDL_gpu's wait has no timeout and a hung GPU would
    // hold the worker forever. After kGpuHung the worker is held (WaitFlight)
    // and polls every kHeldPoll.
    static constexpr std::chrono::microseconds kFencePoll{250};
    static constexpr std::chrono::milliseconds kHeldPoll{10};
    static constexpr std::chrono::milliseconds kGpuHung{2000};

    // A frame drawn, until published. On the zero-copy path it's submitted
    // (RenderFrameToOutput) and the worker moves on: `done` once PollFlight
    // finds its fence signalled; published once done and due.
    struct Drawn {
        std::shared_ptr<const FrameCapture> cap;
        std::chrono::steady_clock::time_point presented;  // by the game
        uint64_t generation = 0;  // the presenting stretch it's drawn for
        bool presenting = false;
        bool new_frame = false;  // not a redraw
        bool pace = false;       // published by PublishPacer
        int slot = -1;           // zero-copy output
        bool drew_output = false, drew_gpu = false, want_rgba = false;
        GpuOutput out;
        GpuStats gs;
        RasterStats rs;
        CameraCuts::Step camera;
        uint32_t width = 0, height = 0;
        bool done = true;
        // steady-clock ns; due_ns is -1 until done
        int64_t submitted_ns = 0, done_ns = 0, due_ns = -1;
    };

    void Run() {
        // sampled in a game stall's log (stall_watch.h)
        stall_watch::SetWorkerThread();
#ifdef _WIN32
        // name decode helpers for profiles
        g_decode_thread_started.store(+[](unsigned index) {
            const std::wstring name = L"band3 decode " + std::to_wstring(index);
            SetThreadDescription(GetCurrentThread(), name.c_str());
        });
#endif
        uint64_t last_frame = 0;
        auto last_dump = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        std::vector<uint32_t> rgba;
        // as of the last settings read; if a capture or change moved it on,
        // the wait below doesn't sleep
        uint64_t epoch = CaptureEpoch();
        // reset per presenting stretch (pacer_generation)
        PublishPacer pacer;
        uint64_t pacer_generation = 0;
        // native_world_ahead's choice, measured per stretch and restore
        AheadChooser ahead_choice;
        uint64_t ahead_generation = 0, ahead_resumes = 0;
        // the last frame's stretch and restores (SetPaused); for a starting
        // stretch, captures seen while waiting for its first
        uint64_t drawn_generation = 0, start_generation = 0, start_seen = 0;
        uint64_t drawn_resumes = 0, start_resumes = 0;
        uint32_t start_waited = 0;
        // LatestCapture's decode ms since the last frame (GpuStats::decode_ms)
        double decode_pending_ms = 0;
        gpu_held_ = false;
        // The zero-copy frame submitted but not published; at most one, so
        // with native_present_pipeline the GPU has at most two worker frames.
        std::optional<Drawn> flight;
        // Pacing: a new frame is due a steady delay after the game presented
        // it (PublishPacer); a redraw at once. The worker publishes sooner on
        // a new capture or a setting or user change.
        auto due_of = [&](const Drawn& d, int64_t done_ns) {
            if (!d.pace || !d.new_frame) return done_ns;
            if (d.generation != pacer_generation) {
                pacer.Reset();
                pacer_generation = d.generation;
            }
            return pacer.Due(d.cap->frame, Nanoseconds(d.presented), done_ns);
        };
        while (true) {
            stall_watch::SetWorker(stall_watch::Worker::kWaiting);
            RasterOptions o;
            std::string dump;
            bool changed, gpu, zero_copy, want_rgba, presenting, pace, pipeline;
            // minimized and no screenshot wanted
            bool paused;
            // presenting stretch (PresentSlots) and restores so far
            uint64_t generation, resumes;
            {
                std::lock_guard lock(mutex_);
                if (stop_) {
                    // a frame the GPU may still be drawing is nobody's
                    if (flight) slots_.Abandon(flight->slot);
                    stall_watch::SetWorker(stall_watch::Worker::kIdle);
                    return;
                }
                o = options_;
                if (present_) {
                    o.width = present_w_;
                    o.height = present_h_;
                    CapHeight(o);
                    band3::aspect::CurrentOverlayEdge(o.overlay_edge);
                    // the in-song HUD under the track runs past 16:9's bottom
                    // by design; show it whole rather than stretched
                    if (InSong()) o.overlay_edge[1] = 1.0f;
                    if (band3::aspect::SongListShowing()) {
                        o.overlay_cut[0] = band3::aspect::kSongListCut[0];
                        o.overlay_cut[1] = band3::aspect::kSongListCut[1];
                    }
                }
                ScaleForPicture(o);
                o.normal_maps = REXCVAR_GET(native_view_normal_maps);
                o.filtering = REXCVAR_GET(native_view_texture_filtering);
                o.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
                // drawn frame after frame, so these have the frames before
                o.trails = true;
                o.post_history = &post_history_;
                o.post_buffer = true;
                o.pre_buffer = true;
                o.gpu_labels = REXCVAR_GET(dred);
                o.gpu_timestamps = REXCVAR_GET(native_gpu_timestamps);
                o.inline_mips = REXCVAR_GET(native_view_inline_mips);
                o.submit_points = uint32_t(REXCVAR_GET(native_view_submit_points));
                o.bc_textures = REXCVAR_GET(native_bc_textures);
                // and k_8's (movie planes) as R8
                o.r8_textures = REXCVAR_GET(native_r8_textures);
                gpu = gpu_;
                dump = dump_path_;
                zero_copy = present_ && present_zero_copy_ && gpu && dump.empty();
                // RGBA for F9's window, a screenshot, the upload path and the CPU
                want_rgba = !zero_copy || dialog_open_ || image_wanted_;
                changed = options_changed_;
                options_changed_ = false;
                presenting = present_;
                paused = presenting && pause_.Paused() && !image_wanted_;
                generation = slots_.Generation();
                resumes = resumes_;
                pace = presenting && REXCVAR_GET(native_present_pacing);
                pipeline = REXCVAR_GET(native_present_pipeline);
                o.gpu_no_wait = pipeline;
                // not with the pipeline, which waits differently
                o.world_ahead = REXCVAR_GET(native_world_ahead) && !pipeline;
                o.world_period = pacing::WorldPeriod();
                // not in menus: textures kept from them into a song kept the
                // texture arrays from ever emptying
                o.clock_keep = InSong();
            }
            // a capture published after this is new (pacing wait, below)
            const uint64_t seen = CaptureEpoch();
            std::chrono::steady_clock::time_point presented;
            // decode left by the game's thread, charged to the next frame
            double decode_ms = 0;
            SetDecodeThreads(uint32_t(REXCVAR_GET(native_deferred_decode_threads)));
            auto cap = LatestCapture(presented, &decode_ms);
            decode_pending_ms += decode_ms;
            bool fresh = true;
            if (cap && presenting) {
                std::lock_guard lock(mutex_);
                fresh = slots_.Fresh(Nanoseconds(presented));
            }
            // A new stretch (or a restore after a pause) starts with a fresh
            // capture that is a whole picture (StartsPicture), or else the
            // kStartWait-th; captures from before don't count.
            if (cap && fresh && presenting &&
                (drawn_generation != generation || drawn_resumes != resumes)) {
                if (start_generation != generation || start_resumes != resumes) {
                    start_generation = generation;
                    start_resumes = resumes;
                    start_waited = 0;
                }
                if (cap->frame != start_seen) {
                    start_seen = cap->frame;
                    start_waited++;
                }
                if (!StartsPicture(*cap) && start_waited < kStartWait) fresh = false;
            }
            const bool work = cap && fresh && (cap->frame != last_frame || changed) && !paused;
            int slot = -1;
            if (flight) {
                // Publish the flight once finished and due (or finished and
                // there's new work). Until then, with native_present_pipeline
                // and a free slot and the GPU not held, record the next one;
                // else wait.
                if (!flight->done) PollFlight(*flight);
                if (flight->done) {
                    if (flight->due_ns < 0) flight->due_ns = due_of(*flight, flight->done_ns);
                    const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
                    if (!work && flight->due_ns > now) {
                        WaitForCapture(seen, std::chrono::nanoseconds(flight->due_ns - now));
                        continue;
                    }
                    PublishDrawn(*flight, nullptr);
                    flight.reset();
                } else {
                    // a flight left as the pipeline turned off is waited for
                    if (work && zero_copy && pipeline && !gpu_held_) {
                        std::lock_guard lock(mutex_);
                        slot = slots_.Acquire();
                    }
                    if (slot < 0) {
                        WaitFlight(*flight, &seen);
                        if (changed) {
                            std::lock_guard lock(mutex_);
                            options_changed_ = true;
                        }
                        continue;
                    }
                }
            }
            if (!work && paused) {
                // Minimized: wake on restore, a screenshot or presenting
                // stopping, not on captures. Settings need no redraw: a
                // restore waits for a new capture anyway.
                std::unique_lock lock(mutex_);
                paint_cv_.wait_for(lock, kIdleWait, [&] {
                    return stop_ || !present_ || !pause_.Paused() || image_wanted_;
                });
                epoch = CaptureEpoch();
                // a decode meanwhile is no frame's
                decode_pending_ms = 0;
                continue;
            }
            if (!work) {
                epoch = WaitForCapture(epoch, kIdleWait);
                continue;
            }
            // an output no paint may still be sampling
            if (zero_copy && slot < 0) {
                slot = AcquireSlot();
                if (slot < 0) {
                    std::lock_guard lock(mutex_);
                    options_changed_ |= changed;
                    continue;
                }
            }
            const bool new_frame = cap->frame != last_frame;
            // captures that came while the last one was drawn
            const uint64_t skipped =
                new_frame && last_frame && cap->frame > last_frame ? cap->frame - last_frame - 1 : 0;
            last_frame = cap->frame;
            if (!o.world_ahead || generation != ahead_generation || resumes != ahead_resumes) {
                ahead_choice.Reset();
                ahead_generation = generation;
                ahead_resumes = resumes;
            }
            if (o.world_ahead && presenting && new_frame)
                ahead_choice.Drawn(Nanoseconds(std::chrono::steady_clock::now()), skipped);
            if (presenting) {
                drawn_generation = generation;
                drawn_resumes = resumes;
            }
            // dumping only saves captures; native_view_replay draws them
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
                decode_pending_ms = 0;
                continue;
            }
            stall_watch::SetWorker(stall_watch::Worker::kRecording);
            Drawn d;
            d.cap = cap;
            d.presented = presented;
            d.generation = generation;
            d.presenting = presenting;
            d.new_frame = new_frame;
            d.pace = pace;
            d.slot = slot;
            d.want_rgba = want_rgba;
            d.width = o.width;
            d.height = o.height;
            d.camera = cuts_.Next(*cap);
            // without a device RenderFrame fails (for good) and the CPU draws
            if (slot >= 0) {
                d.drew_output = GpuRenderer::Get().RenderFrameToOutput(*cap, o, slot, d.out, d.gs);
                d.drew_gpu = d.drew_output;
                d.submitted_ns = Nanoseconds(std::chrono::steady_clock::now());
                if (d.drew_output) {
                    std::lock_guard lock(mutex_);
                    slots_.Submitted(slot);
                    d.done = false;
                    if (live_measuring_)
                        live_.in_flight_max =
                            std::max(live_.in_flight_max, uint32_t(slots_.InFlight()));
                }
                if (d.drew_output && !pipeline) WaitFlight(d, nullptr);
                // an output that failed the zero-copy checks is read back (the
                // drawer then uploads); reading back waits for the GPU
                if (d.drew_output && (d.want_rgba || !d.out.d3d12_resource)) {
                    uint32_t w = 0, h = 0;
                    d.drew_gpu = GpuRenderer::Get().DownloadOutput(slot, rgba, w, h);
                    d.want_rgba = true;
                    WaitFlight(d, nullptr);
                }
            } else if (gpu) {
                d.drew_gpu = GpuRenderer::Get().RenderFrame(*cap, o, rgba, d.gs);
            }
            if (!d.drew_gpu) {
                d.rs = Rasterize(*cap, o, rgba);
                d.want_rgba = true;
            }
            // the deferred decode counts toward the frame's time
            d.gs.decode_ms = decode_pending_ms;
            if (d.drew_gpu) d.gs.ms += decode_pending_ms;
            else d.rs.ms += decode_pending_ms;
            decode_pending_ms = 0;
            if (flight && slot >= 0 && !d.drew_output) {
                // the GPU failed and gpu_view released the device, the
                // previous output with it
                std::lock_guard lock(mutex_);
                slots_.Abandon(flight->slot);
                flight.reset();
            }
            if (flight) {
                // the previous frame: publish as soon as it finishes, in
                // order for the pacer, as this one is newer
                if (!WaitFlight(*flight, nullptr)) {
                    // stopping: both are nobody's
                    if (!d.done) {
                        std::lock_guard lock(mutex_);
                        slots_.Abandon(d.slot);
                    }
                    continue;
                }
                flight->due_ns = due_of(*flight, flight->done_ns);
                PublishDrawn(*flight, nullptr);
                flight.reset();
            }
            if (!d.done) {
                // in flight until the GPU finishes it, or stopping before a
                // read-back frame finished
                if (!d.want_rgba) {
                    flight = std::move(d);
                } else {
                    std::lock_guard lock(mutex_);
                    slots_.Abandon(d.slot);
                }
                continue;
            }
            const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
            // native_world_ahead: draw a world frame's world now, while this
            // frame waits to publish. `now` is taken first so the pacer's
            // delay doesn't grow. Gated by AheadChooser (ahead_gated).
            if (o.world_ahead && d.drew_gpu && KindOf(*cap) == FrameKind::kWorld) {
                if (ahead_choice.On()) {
                    GpuStats ahead;
                    if (GpuRenderer::Get().RenderWorldAhead(*cap, o, ahead))
                        d.gs.ahead_ms = ahead.ms;
                } else {
                    d.gs.ahead_gated = 1;
                }
            }
            const int64_t due = due_of(d, now);
            const int64_t after = Nanoseconds(std::chrono::steady_clock::now());
            if (due > after) WaitForCapture(seen, std::chrono::nanoseconds(due - after));
            PublishDrawn(d, d.want_rgba ? &rgba : nullptr);
        }
    }

    // One fence check; when finished, marks the slot finished and un-holds
    // the worker.
    bool PollFlight(Drawn& f) {
        if (f.done) return true;
        // also fills its GPU timings
        if (!GpuRenderer::Get().OutputDone(f.slot, &f.gs)) return false;
        f.done = true;
        f.done_ns = Nanoseconds(std::chrono::steady_clock::now());
        if (gpu_held_) {
            gpu_held_ = false;
            REXLOG_INFO("native renderer: the GPU finished the frame after {} ms; drawing again",
                        (f.done_ns - f.submitted_ns) / 1000000);
        }
        std::lock_guard lock(mutex_);
        slots_.Finished(f.slot);
        return true;
    }

    // Waits for `f` (true), until stopping or, with `seen`, CaptureEpoch
    // moves on; adds to wait_ms. Past kGpuHung the worker is held, submitting
    // nothing more until the GPU finishes, rather than pile frames onto a GPU
    // that may have hung.
    bool WaitFlight(Drawn& f, const uint64_t* seen) {
        stall_watch::SetWorker(stall_watch::Worker::kGpuWait);
        const auto from = std::chrono::steady_clock::now();
        while (!PollFlight(f)) {
            {
                std::lock_guard lock(mutex_);
                if (stop_) break;
            }
            if (seen && CaptureEpoch() != *seen) break;
            const int64_t waited_ms =
                (Nanoseconds(std::chrono::steady_clock::now()) - f.submitted_ns) / 1000000;
            if (!gpu_held_ && waited_ms >= kGpuHung.count()) {
                gpu_held_ = true;
                REXLOG_WARN("native renderer: the GPU hasn't finished a frame in {} ms; holding "
                            "the last frame",
                            waited_ms);
            }
            pacing::SleepFor(gpu_held_ ? std::chrono::nanoseconds(kHeldPoll).count()
                                       : std::chrono::nanoseconds(kFencePoll).count());
        }
        f.gs.wait_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - from)
                            .count();
        return f.done;
    }

    // Hands `d` to the window (if still its stretch), live stats, and RGBA
    // users. A slot's frame must be done.
    void PublishDrawn(const Drawn& d, const std::vector<uint32_t>* rgba) {
        // the description costs a string per draw; only F9's window reads it
        const bool describe = dialog_open_.load(std::memory_order_relaxed);
        const std::string stats =
            describe ? Describe(*d.cap, d.drew_gpu ? DescribeGpu(d.gs, d.slot >= 0)
                                                   : DescribeRaster(d.rs))
                     : std::string();
        if (d.new_frame) {
            const uint64_t skipped =
                published_frame_ && d.cap->frame > published_frame_
                    ? d.cap->frame - published_frame_ - 1
                    : 0;
            published_frame_ = std::max(published_frame_, d.cap->frame);
            const int32_t slow_ms = REXCVAR_GET(native_slow_frame_ms);
            if (slow_ms > 0 && d.drew_gpu && d.gs.ms > slow_ms) LogSlow(d, skipped);
        }
        std::unique_lock lock(mutex_);
        // Never a frame the GPU may still be drawing: Publish would refuse it,
        // and abandoning it would free a slot the GPU is still writing. Leave
        // it taken.
        if (d.slot >= 0 && (!d.done || slots_.Unfinished(d.slot))) {
            if (!unfinished_logged_) {
                unfinished_logged_ = true;
                REXLOG_WARN("native renderer: a frame was to be published before the GPU had "
                            "finished it; left unpublished");
            }
            return;
        }
        const bool current = d.presenting && d.generation == slots_.Generation();
        // called after unlocking (SetPublished)
        std::shared_ptr<const std::function<void()>> published;
        bool to_window = false;
        if (d.slot >= 0) {
            if (d.drew_output && slots_.Publish(d.slot, d.generation)) {
                outputs_[d.slot] = d.out;
                slot_serial_[d.slot] = slots_.Serial();
                slot_presented_[d.slot] = d.presented;
                published = published_;
                to_window = true;
            } else {
                slots_.Abandon(d.slot);
            }
        }
        // captures from before the stats restarted, or redraws, don't count
        if (live_measuring_ && d.cap->frame > live_last_) {
            const uint64_t skipped = d.cap->frame - live_last_ - 1;
            live_.skipped_busy += skipped;
            live_last_ = d.cap->frame;
            live_.rendered++;
            if (ProcKnown(*d.cap) && !DrawsWorld(*d.cap) && !d.cap->composed) live_.worldless++;
            const double ms = d.drew_gpu ? d.gs.ms : d.rs.ms;
            live_.ms.push_back(ms);
            if (d.drew_gpu) live_.wait_ms.push_back(d.gs.wait_ms);
            // skips are charged to the previous frame's kind, being drawn
            // when they came
            const int k = int(KindOf(*d.cap));
            LiveViewStats::Kind& kind = live_.by_kind[k];
            live_.by_kind[live_last_kind_ >= 0 ? live_last_kind_ : k].skipped_busy += skipped;
            live_last_kind_ = k;
            kind.rendered++;
            kind.ms.push_back(ms);
            if (d.cap->composed) kind.composed++;
            AddCost(kind.cost, d.cap->cost);
            if (d.camera.cut) live_.camera_cuts++;
            if (d.camera.vel_reset) live_.vel_resets++;
            if (d.camera.confirmed) live_.cuts_confirmed++;
            if (d.drew_gpu) {
                kind.wait_ms.push_back(d.gs.wait_ms);
                kind.gpu_frames++;
                if (d.gs.gpu_timed) {
                    kind.gpu_timed++;
                    kind.gpu_total_ms.push_back(d.gs.gpu_total_ms);
                }
                if (d.gs.shows_kept) kind.shows_kept++;
                if (d.gs.arena_rebuilt) kind.arena_rebuilt++;
                AddGpu(kind.gpu, d.gs);
                MostPlan(kind.plan_most, d.gs);
                if (d.gs.plan_ms > LiveViewStats::kPlanSpikeMs) {
                    kind.plan_spikes++;
                    if (d.camera.since_cut >= 0 &&
                        d.camera.since_cut <= LiveViewStats::kSpikeAfterCut)
                        kind.plan_spikes_at_cut++;
                }
                kind.peak_meshes = std::max(kind.peak_meshes, d.gs.resident_meshes);
                kind.peak_textures = std::max(kind.peak_textures, d.gs.resident_textures);
                kind.peak_rts = std::max(kind.peak_rts, d.gs.resident_rts);
                kind.peak_texture_array_mb =
                    std::max(kind.peak_texture_array_mb, d.gs.texture_array_mb);
                kind.peak_arena_mb = std::max(kind.peak_arena_mb, d.gs.arena_mb);
                kind.peak_rts_mb = std::max(kind.peak_rts_mb, d.gs.rts_mb);
                kind.peak_meshes_by_time =
                    std::max(kind.peak_meshes_by_time, d.gs.meshes_by_time);
                kind.peak_textures_by_time =
                    std::max(kind.peak_textures_by_time, d.gs.textures_by_time);
            }
        }
        live_drew_gpu_ = d.drew_gpu;
        if (rgba) {
            image_ = *rgba;
            image_w_ = d.width;
            image_h_ = d.height;
            image_presented_ = d.presented;
            image_serial_++;
            image_generation_ = current ? d.generation : 0;
            // while presenting, a screenshot must come from the window's
            // stretch (Take's `for_present`)
            if (current || !present_) image_wanted_ = false;
            // upload path
            if (current && d.slot < 0) {
                published = published_;
                to_window = true;
            }
        }
        if (describe) stats_ = stats;
        lock.unlock();
        // redraws aren't timed
        if (to_window && d.new_frame) NotePublished(d.presented);
        if (published) (*published)();
    }

    // at most kSlowPerSecond lines a second; the next one counts those left out
    static constexpr uint32_t kSlowPerSecond = 2;
    void LogSlow(const Drawn& d, uint64_t skipped) {
        const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
        if (now - slow_since_ns_ >= 1'000'000'000) {
            slow_since_ns_ = now;
            slow_logged_ = 0;
        }
        if (slow_logged_ >= kSlowPerSecond) {
            slow_left_out_++;
            return;
        }
        slow_logged_++;
        std::string line = DescribeSlow(*d.cap, d.gs, skipped, d.camera);
        if (slow_left_out_) {
            line += " (" + std::to_string(slow_left_out_) + " more left out before it)";
            slow_left_out_ = 0;
        }
        REXLOG_INFO("{}", line);
    }

    // A free slot, or -1 after waiting for one. Once paints have stopped for
    // kPaintsStopped, settle_ asks the GPU whether it finished them all,
    // freeing every slot they sampled.
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
        // Only a paint frees a slot (ShowNewest's completed index): wait for
        // one, or until settling can be tried. Without a working fence, poll
        // every kIdleWait rather than spin.
        std::unique_lock lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto until = now + kIdleWait;
        if (settle_ && !settle) until = std::min(until, std::max(now, last_paint_ + kPaintsStopped));
        paint_cv_.wait_until(lock, until, [&] { return stop_ || !present_ || paints_ != paints; });
        return -1;
    }

    std::mutex mutex_;
    std::thread thread_;
    // the CPU's post and pre-process buffers; worker only
    post::PostHistory post_history_;
    int users_ = 0;
    bool stop_ = false;
    RasterOptions options_;
    bool gpu_ = false;
    bool options_changed_ = false;
    // set under mutex_; atomic for PublishDrawn's read before locking
    std::atomic<bool> dialog_open_{false};
    std::string dump_path_;
    std::vector<uint32_t> image_;
    uint32_t image_w_ = 0, image_h_ = 0;
    uint64_t image_serial_ = 0;
    // PresentSlots::Generation it was drawn for, 0 none
    uint64_t image_generation_ = 0;
    std::chrono::steady_clock::time_point image_presented_{};
    bool image_wanted_ = false;
    uint32_t dump_count_ = 0;
    std::string stats_;
    bool present_ = false;
    uint32_t present_w_ = 1280, present_h_ = 720;
    bool present_zero_copy_ = false;
    PresentSlots slots_;
    DrawPause pause_;
    uint64_t resumes_ = 0;
    GpuOutput outputs_[PresentSlots::kCount];
    uint64_t slot_serial_[PresentSlots::kCount] = {};
    std::chrono::steady_clock::time_point slot_presented_[PresentSlots::kCount] = {};
    std::chrono::steady_clock::time_point last_paint_{};
    uint64_t paints_ = 0;
    std::condition_variable paint_cv_;
    std::function<bool()> settle_;
    std::shared_ptr<const std::function<void()>> published_;
    // worker only (WaitFlight)
    bool gpu_held_ = false;
    bool unfinished_logged_ = false;
    // captures up to live_base_ predate the stats; live_last_ is the newest
    // counted
    bool live_measuring_ = false;
    LiveViewStats live_;
    uint64_t live_base_ = 0, live_last_ = 0;
    // -1 none yet
    int live_last_kind_ = -1;
    std::optional<bool> live_drew_gpu_;
    // worker only (LogSlow)
    uint64_t published_frame_ = 0;
    int64_t slow_since_ns_ = 0;
    uint32_t slow_logged_ = 0, slow_left_out_ = 0;
    // worker only
    CameraCuts cuts_;
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
    // on the UI thread, where SDL wants its video started
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
        changed |= ImGui::Checkbox("Post-processing", &options_.post);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Gamma ramp", &options_.gamma);
        // the native renderer owns the size while it draws the window
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
        // F4 can change it too
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
    // on the UI thread, where SDL wants its video started
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
// the native renderer on the window

namespace {

// For present_stats: the drawer's Draw notes every paint, either renderer, on
// the UI thread.
std::mutex g_paints_mutex;
PaintRecorder g_paints;
std::chrono::steady_clock::time_point g_paints_since = std::chrono::steady_clock::now();
std::string g_present_path;

// with `native`: showing frame `serial` (0 none) of `source`'s numbering
void NotePaint(std::chrono::steady_clock::time_point now, bool native, int source = 0,
               uint64_t serial = 0, std::chrono::steady_clock::time_point presented = {}) {
    std::lock_guard lock(g_paints_mutex);
    g_paints.Paint(Nanoseconds(now), native, source, serial,
                   presented.time_since_epoch().count() ? Nanoseconds(presented) : 0);
}

void NotePublished(std::chrono::steady_clock::time_point presented) {
    const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
    std::lock_guard lock(g_paints_mutex);
    g_paints.Published(now, presented.time_since_epoch().count() ? Nanoseconds(presented) : 0);
}

// PaintRecorder sources; zero-copy is D3D12 only
[[maybe_unused]] constexpr int kZeroCopyFrames = 0;
constexpr int kUploadedFrames = 1;

// the SDK's present_letterbox (on if absent); off under native_fill_window
bool PresentLetterbox() {
    if (REXCVAR_GET(native_fill_window)) return false;
    return !rex::cvar::GetFlagInfo("present_letterbox") ||
           rex::cvar::Query<bool>("present_letterbox");
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// Whether the GPU finished every paint so far: band3's own fence signalled on
// the SDK's direct queue. Only signals, never waits on that queue (the
// emulated GPU submits to it). Shared because the worker (SetSettle) may still
// call it while the drawer stops.
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

    // true if everything submitted before the call finished within the timeout
    bool Settle(DWORD timeout_ms) {
        std::lock_guard lock(mutex_);
        const UINT64 value = ++value_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) return false;
        if (fence_->GetCompletedValue() < value &&
            SUCCEEDED(fence_->SetEventOnCompletion(value, event_)))
            WaitForSingleObject(event_, timeout_ms);
        // check the fence: a timed-out wait leaves the event set
        return fence_->GetCompletedValue() >= value;
    }

 private:
    std::mutex mutex_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 value_ = 0;
};

// Zero-copy drawing: present.hlsl's triangle over the presenter's back buffer
// on its own command list, sampling gpu_view's outputs through band3's own
// shader-visible heap (a descriptor per output). The presenter binds the back
// buffer; the SDK's immediate drawer (ImGui, z 64) resets its own state after
// (verified by the N2 kill test).
class D3D12Present {
 public:
    bool Init(const rex::ui::d3d12::D3D12Provider& provider) {
        device_ = provider.GetDevice();
        HRESULT hr;
        {
            // b0: PresentConstants (8); t0: the frame via the heap; s0: linear
            // clamp
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

    void Draw(rex::ui::d3d12::D3D12UIDrawContext& ctx, const ImageRect& rect, int slot,
              const GpuOutput& out) {
        ID3D12GraphicsCommandList* cl = ctx.command_list();
        const uint32_t w = ctx.render_target_width(), h = ctx.render_target_height();
        if (!cl || !w || !h || !rect.w || !rect.h) return;
        // A new texture in the slot: overwrite the view. Safe because the
        // worker only reuses slots no unfinished paint sampled (PresentSlots).
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
    // holds each output's texture while paints may sample it (SDL drops its
    // own reference when it remakes the slot)
    struct View {
        ComPtr<ID3D12Resource> resource;
        uint64_t generation = 0;
    };
    View views_[GpuRenderer::kOutputs];
};
#endif

// UI thread: re-Follow once the emulated picture is fresh, if the drawer
// still exists
void FollowRendererLater();

// The UI drawer at z 0, under ImGui (64); draws nothing while emulated. While
// native it sets the worker's size (LetterboxRect) and draws the newest frame
// each paint: zero-copy on D3D12, else uploaded via the immediate drawer.
class NativePresentDrawer : public rex::ui::UIDrawer {
 public:
    using ImmediateGetter = std::function<rex::ui::ImmediateDrawer*()>;
    NativePresentDrawer(rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
                        rex::ui::Window* window, ImmediateGetter immediate)
        : presenter_(presenter), provider_(provider), window_(window),
          immediate_(std::move(immediate)) {}

    void Start() {
#ifdef _WIN32
        // On Windows the SDK's only provider is D3D12 (rexruntime.dll exports
        // no Vulkan).
        if (provider_) {
            const auto& d3d12 = static_cast<const rex::ui::d3d12::D3D12Provider&>(*provider_);
            // WARP (and the Basic Render Driver): sharing the SDK's device
            // with the emulated GPU faults inside WARP and kills band3.
            // Hardware sharing is fine.
            if (d3d12.GetAdapterVendorID() == rex::ui::GraphicsProvider::GpuVendorID::kMicrosoft)
                GpuRenderer::Get().RefuseDevice(
                    "the SDK's GPU is Microsoft's software rasterizer (WARP), whose Direct3D 12 "
                    "device band3's GPU drawing would share with the emulated GPU, and that "
                    "crashes there");
            // timestamp ticks per second, for native_gpu_timestamps
            UINT64 frequency = 0;
            ID3D12CommandQueue* queue = d3d12.GetDirectQueue();
            if (!queue || FAILED(queue->GetTimestampFrequency(&frequency))) frequency = 0;
            GpuRenderer::Get().SetPresentDevice(d3d12.GetDevice(), frequency);
            // DRED on device removal, either renderer; the only place band3
            // has the SDK's device from the start
            crash_trace::WatchD3D12Device(d3d12.GetDevice(), d3d12.GetDirectQueue());
            // RasterOptions::gpu_labels
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
        // F8 back to emulated waits for a fresh emulated picture; signalled
        // on the game's thread, handled on the UI thread
        if (window_) {
            rex::ui::WindowedAppContext* app = &window_->app_context();
            SetEmulatedFreshCallback(
                [app] { app->CallInUIThreadDeferred([] { FollowRendererLater(); }); });
        }
        // F8, F4 and the harness's `set` change the picture on the UI thread
        picture_listener_ = AddShownPictureListener([this](bool native) { Follow(native); });
        Follow(ShowsNativePicture());
        // With renderer native no emulated swap requests paints, so each
        // published frame does; one pending request covers frames published
        // before it runs. None while the window can't be seen
        // (PaintTarget::Seen), or it kept painting minimized.
        if (sync_gpu::NativeOnly() && REXCVAR_GET(native_present_request_paint) && window_) {
            paint_target_ = std::make_shared<PaintTarget>();
            paint_target_->window = window_;
            rex::ui::WindowedAppContext* app = &window_->app_context();
            std::shared_ptr<PaintTarget> target = paint_target_;
            Renderer::Get().SetPublished([app, target] {
                if (target->pending.exchange(true)) return;
                app->CallInUIThread([target] {
                    target->pending.store(false);
                    if (target->window && target->Seen())
                        target->window->RequestPresenterUIPaintFromUIThread();
                });
            });
            REXLOG_INFO("native present: asking for a paint with each frame (renderer native)");
        }
    }

    void Stop() {
        if (paint_target_) {
            Renderer::Get().SetPublished(nullptr);
            // a request still queued finds no window (both on the UI thread)
            paint_target_->window = nullptr;
            paint_target_.reset();
        }
        SetEmulatedFreshCallback(nullptr);
        RemoveShownPictureListener(picture_listener_);
        presenter_->RemoveUIDrawerFromUIThread(this);
        Follow(false, true);
#ifdef _WIN32
        // let the GPU finish paints that sampled outputs before releasing
        // them (no paint runs concurrently: this is the UI thread)
        if (fence_ && drew_) fence_->Settle(2000);
        d3d12_.reset();
        crash_trace::WatchD3D12Device(nullptr, nullptr);
        crash_trace::SetIndexedDrawNamer(nullptr);
#endif
        for (auto& t : textures_) t.reset();
    }

    // UI thread. Renderer native pauses the worker while minimized; on
    // restore, request a paint at once rather than wait a frame.
    void SetMinimized(bool minimized) {
        if (sync_gpu::NativeOnly()) Renderer::Get().SetPaused(minimized);
        if (!paint_target_) return;
        paint_target_->minimized = minimized;
        if (!minimized && paint_target_->window && paint_target_->Seen())
            paint_target_->window->RequestPresenterUIPaintFromUIThread();
    }

    // Tracks the shown picture (renderer_switch.h), UI thread. Turning
    // native starts the worker. Turning emulated keeps drawing until the
    // emulated GPU, which skipped draws (gpu_skip.h), has whole frames again
    // (SkipLatch::Fresh; gpu_skip.cpp calls back) or kMaxDrain passes.
    // `at_once`: shutdown, no wait. With renderer native, always native.
    void Follow(bool native, bool at_once = false) {
        if (!at_once && sync_gpu::NativeOnly()) native = true;
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
            // the emulated picture is 16:9
            band3::aspect::SetWindow(0, 0, false);
            REXLOG_INFO("native present: off, the emulated GPU's picture shows (after {:.0f} ms)",
                        waited);
            // whole frames don't bring back what RB3 drew once (gpu_skip.h)
            const uint64_t dropped = GetGpuSkipStats().passes_dropped - passes_dropped_on_;
            if (dropped && !at_once) {
                REXLOG_WARN("native present: {} texture passes RB3 draws once were skipped "
                            "(swap_only): outfits/portraits may be wrong until RB3 draws them "
                            "again",
                            dropped);
            }
            return;
        }
        active_ = true;
        passes_dropped_on_ = GetGpuSkipStats().passes_dropped;
        // make pipelines now rather than stall the worker's first frame
        if (UpdateGpu()) GpuRenderer::Get().Prewarm(uint32_t(REXCVAR_GET(native_view_msaa)));
        const bool zero_copy = ChoosePath();
        // size from the window, as a paint may be long in coming (minimized)
        const uint32_t w = window_ ? window_->GetActualPhysicalWidth() : 0;
        const uint32_t h = window_ ? window_->GetActualPhysicalHeight() : 0;
        band3::aspect::SetWindow(w, h, REXCVAR_GET(native_fill_window));
        ImageRect rect = rect_;
        if (!rect.w || !rect.h)
            rect = w && h ? LetterboxRect(w, h, PresentLetterbox()) : ImageRect{0, 0, 1280, 720};
        Renderer::Get().StartPresent(rect.w, rect.h, zero_copy);
        // the upload path's last picture is stale
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
        // every paint is timed, either renderer (present_stats)
        const auto now = std::chrono::steady_clock::now();
        // also polled each paint, besides the listener
        Follow(ShowsNativePicture());
        if (!active_) {
            NotePaint(now, false);
            return;
        }
        const uint32_t tw = context.render_target_width(), th = context.render_target_height();
        if (!tw || !th) {
            NotePaint(now, true);
            return;
        }
        band3::aspect::SetWindow(tw, th, REXCVAR_GET(native_fill_window));
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
    // native_view_backend; the device starts here, on the UI thread
    bool UpdateGpu() {
        const bool gpu = REXCVAR_GET(native_view_backend) == "gpu" && GpuRenderer::Get().Init();
        Renderer::Get().SetGpu(gpu);
        return gpu;
    }

    // logged when it changes
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

    // Two textures alternate so the one a paint in flight reads isn't freed.
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
            // immediate textures can't be updated, only created
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
            // black bars
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
    // turned emulated; drawing on until the emulated picture is fresh (Follow)
    bool draining_ = false;
    std::chrono::steady_clock::time_point drain_start_;
    static constexpr std::chrono::milliseconds kMaxDrain{3000};
    // GpuSkipStats::passes_dropped when it last turned native
    uint64_t passes_dropped_on_ = 0;
    // at the last paint
    ImageRect rect_;
    bool path_logged_ = false;
    bool zero_copy_ = false;
    std::string why_;
    // upload path
    std::unique_ptr<rex::ui::ImmediateTexture> textures_[2];
    int current_ = 0;
    uint64_t upload_frame_ = 0;
    // a picture taken since native last started
    bool upload_shown_ = false;
    std::chrono::steady_clock::time_point upload_presented_{};
    std::vector<uint32_t> upload_;
    int picture_listener_ = 0;
    // renderer native: the window to request paints from (window is read and
    // cleared on the UI thread) and whether a request is pending
    struct PaintTarget {
        rex::ui::Window* window = nullptr;
        std::atomic<bool> pending{false};
        // UI thread only; `held` logs changes
        bool minimized = false;
        bool held = false;

        // PaintWanted from the OS where it can be asked (Windows), else the
        // SDK's minimized flag
        bool Seen() {
            launcher::WindowShown shown;
            if (!launcher::NativeWindowShown(window->GetNativeWindowHandle(), shown)) {
                shown.minimized = minimized;
                shown.client_w = window->GetActualPhysicalWidth();
                shown.client_h = window->GetActualPhysicalHeight();
            }
            const bool seen =
                PaintWanted(shown.minimized, shown.visible, shown.client_w, shown.client_h);
            if (seen == held) {
                held = !seen;
                REXLOG_INFO("native present: {} (minimized {}, visible {}, client {}x{})",
                            seen ? "the window can be seen again, asking for paints"
                                 : "the window can't be seen, asking for no paints",
                            shown.minimized, shown.visible, shown.client_w, shown.client_h);
            }
            return seen;
        }
    };
    std::shared_ptr<PaintTarget> paint_target_;
#ifdef _WIN32
    std::unique_ptr<D3D12Present> d3d12_;
    std::string d3d12_why_;
    std::shared_ptr<PaintFence> fence_;
    bool drew_ = false;  // a paint sampled an output
#endif
};

std::unique_ptr<NativePresentDrawer> g_present;

void FollowRendererLater() {
    if (g_present) g_present->Follow(ShowsNativePicture());
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

void NativePresentMinimized(bool minimized) {
    if (g_present) g_present->SetMinimized(minimized);
}

bool NativePresenting() { return Renderer::Get().Presenting(); }

namespace {
std::atomic<bool> g_in_song{false};
}  // namespace
void SetInSong(bool in_song) { g_in_song.store(in_song, std::memory_order_relaxed); }
bool InSong() { return g_in_song.load(std::memory_order_relaxed); }

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

double RecentPaintLatencyMs() {
    if (!Renderer::Get().Presenting()) return 0;
    std::lock_guard lock(g_paints_mutex);
    return g_paints.RecentLatencyMs();
}

bool NativePresentDrawSize(uint32_t& width, uint32_t& height) {
    return Renderer::Get().PresentDrawSize(width, height);
}

void ScaleForPicture(RasterOptions& o) {
    // see PassTargetSize
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
