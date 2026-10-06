#include "src/Render/native_view.h"

#include "src/Hooks/frame_pacing.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Render/camera_cut.h"
#include "src/Render/capture_file.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/gpu_view.h"
#include "src/Render/png_writer.h"
#include "src/Render/post_model.h"
#include "src/Render/present_model.h"
#include "src/Render/sync_gpu/emulated_gpu_mode.h"
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

// `output`: drawn into the presenter's output (RenderFrameToOutput), whose
// ms is up to submitting and wait_ms the worker's wait for the GPU after
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

// the worker handed the window a new frame the game presented at `presented`,
// for present_stats' publish latency (below, with the paints)
void NotePublished(std::chrono::steady_clock::time_point presented);

// `g` added into the totals `sum` (LiveViewStats::Kind's)
void AddGpu(GpuStats& sum, const GpuStats& g) {
    sum.draws += g.draws;
    sum.skipped += g.skipped;
    sum.uploads += g.uploads;
    sum.passes += g.passes;
    sum.rt_missing += g.rt_missing;
    sum.ms += g.ms;
    sum.wait_ms += g.wait_ms;
    sum.pre_ms += g.pre_ms;
    sum.plan_ms += g.plan_ms;
    sum.upload_ms += g.upload_ms;
    sum.record_ms += g.record_ms;
    sum.submit_ms += g.submit_ms;
    sum.evict_ms += g.evict_ms;
    sum.pre_passes += g.pre_passes;
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
    sum.pool_meshes += g.pool_meshes;
    sum.arena_moved += g.arena_moved;
    sum.arena_sent += g.arena_sent;
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
    sum.texture_array_mb += g.texture_array_mb;
    sum.arena_mb += g.arena_mb;
    sum.rts_mb += g.rts_mb;
}

// the most of plan_ms and its parts in `most` (LiveViewStats::Kind's
// plan_most) or `g`'s
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
    sum.geom_miss_bytes += c.geom_miss_bytes;
    sum.tex_decode_bytes += c.tex_decode_bytes;
    sum.game_ns += c.game_ns;
}

// native_slow_frame_ms's line's plan parts and camera, for DescribeSlow
std::string DescribePlan(const GpuStats& gs, const CameraCuts::Step& cam) {
    // the buffers reserve grew
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
    char arena[48] = "";
    if (gs.arena_rebuilt) std::snprintf(arena, sizeof(arena), " (rebuilt, %.1f MB)", gs.arena_new_mb);
    char buf[640];
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
    // camera_cut.h's view of it
    if (!cam.world_frame) {
        s += "; camera: none in its world";
    } else if (!cam.new_world) {
        std::snprintf(buf, sizeof(buf), "; camera: game frame %llu's world, drawn before",
                      static_cast<unsigned long long>(cam.world_frame));
        s += buf;
    } else if (cam.jump >= 0 || cam.turn >= 0) {
        // (-1 for the one that can't be told)
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

// native_slow_frame_ms's line for a GPU frame `fc` that took `gs`, after
// `skipped` captures the worker never drew, its camera as `cam` says
std::string DescribeSlow(const FrameCapture& fc, const GpuStats& gs, uint64_t skipped,
                         const CameraCuts::Step& cam) {
    uint64_t hooks_ns = 0;
    for (uint64_t ns : fc.cost.hook_ns) hooks_ns += ns;
    const FrameKind kind = KindOf(fc);
    char world[64] = "";
    if (fc.composed)
        std::snprintf(world, sizeof(world), ", world from game frame %llu",
                      static_cast<unsigned long long>(fc.world_frame));
    char buf[1400];
    std::snprintf(
        buf, sizeof(buf),
        "native renderer: slow frame %llu (game frame %llu, %s, proc_cmds %u%s): %.1f ms, "
        "%.1f of it waiting for the GPU; pre %.1f (%u passes), plan %.1f, upload %.1f, record "
        "%.1f, submit %.1f, evict %.1f ms; %u draws (%u of the world%s), %u texture passes; "
        "sent %u meshes into the pool and %u into the arena (%.2f MB), %u textures (%.2f MB), "
        "%u KB of bones; moved %u meshes to the arena%s; made %u pipelines, %u buffers, %u "
        "textures; let go of %u meshes, %u textures, %u targets' pictures after, and released "
        "%u targets (%.1f MB of targets kept); its capture cost the game's thread %.2f ms "
        "(%u draws, %u new shades, %u allocations, %u bones, %llu KB of "
        "geometry and %llu KB of textures decoded) in a %.1f ms game frame; %llu captures "
        "skipped before it",
        static_cast<unsigned long long>(fc.frame), static_cast<unsigned long long>(fc.game_frame),
        FrameKindName(kind), fc.proc_cmds, world, gs.ms, gs.wait_ms, gs.pre_ms, gs.pre_passes,
        gs.plan_ms, gs.upload_ms, gs.record_ms, gs.submit_ms, gs.evict_ms, gs.draws,
        gs.world_draws, gs.shows_kept ? ", the kept post buffer shown" : "", gs.passes,
        gs.pool_meshes, gs.arena_sent, gs.mesh_bytes / 1048576.0, gs.textures_sent,
        gs.texture_bytes / 1048576.0, uint32_t(gs.bone_bytes >> 10), gs.arena_moved,
        gs.arena_rebuilt ? " (rebuilt it)" : "", gs.pipelines_made, gs.buffers_made,
        gs.textures_made, gs.evicted_meshes, gs.evicted_textures, gs.evicted_rts,
        gs.rts_released, gs.rts_mb, hooks_ns / 1e6, fc.cost.draws, fc.cost.new_shades,
        fc.cost.allocs, fc.cost.bones,
        static_cast<unsigned long long>(fc.cost.geom_miss_bytes >> 10),
        static_cast<unsigned long long>(fc.cost.tex_decode_bytes >> 10), fc.cost.game_ns / 1e6,
        static_cast<unsigned long long>(skipped));
    return buf + DescribePlan(gs, cam);
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
    // With emulated_gpu off, the window minimized (`paused`) or restored
    // (DrawPause): while it's minimized the worker draws nothing for it but a
    // screenshot asked for, keeping its outputs, and the captures the game
    // publishes meanwhile go undrawn. Restored, the window goes on showing
    // the last frame published (no new stretch: nothing would show, black
    // with emulated_gpu off) until one from a capture published from then on
    // is (PresentSlots::Resume); and that first one is a whole picture, as a
    // stretch's first is (Run: the kept post buffer is from before the pause).
    void SetPaused(bool paused) {
        auto cap = LatestCapture();
        const uint64_t frame = cap ? cap->frame : 0;
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
                // the captures that came meanwhile weren't skipped for being
                // busy (LiveViewStats::skipped_busy)
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
    // called on the worker each time a frame is published for the window
    // (emulated_gpu off's paint request); null to stop
    void SetPublished(std::function<void()> published) {
        std::lock_guard lock(mutex_);
        published_ = published ? std::make_shared<const std::function<void()>>(std::move(published))
                               : nullptr;
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
        // a worker paused (minimized) waits on this instead
        paint_cv_.notify_all();
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
        live_last_kind_ = -1;
        live_drew_gpu_.reset();
        pause_.Restart(Nanoseconds(std::chrono::steady_clock::now()), live_base_);
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
        s.paused = pause_.Paused();
        s.paused_ms = pause_.Ms(Nanoseconds(std::chrono::steady_clock::now()));
        s.paused_captures = pause_.Captures(cap ? cap->frame : 0);
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
    // How often the worker looks at a submitted frame's fence while it waits
    // for the GPU to finish it: SDL_gpu's own wait has no timeout, and a hung
    // GPU would hold the worker in it for good. A frame unfinished after
    // kGpuHung holds the worker (WaitFlight), which then looks every
    // kHeldPoll.
    static constexpr std::chrono::microseconds kFencePoll{250};
    static constexpr std::chrono::milliseconds kHeldPoll{10};
    static constexpr std::chrono::milliseconds kGpuHung{2000};

    // A frame drawn, until it's published: what it was drawn from and for,
    // how, and its numbers. On the zero-copy path it's submitted to the GPU
    // (RenderFrameToOutput) and the worker goes on while the GPU draws it:
    // `done` once a look at its fence finds it finished (PollFlight), and
    // published once done and due.
    struct Drawn {
        std::shared_ptr<const FrameCapture> cap;
        std::chrono::steady_clock::time_point presented;  // by the game
        uint64_t generation = 0;  // the presenting stretch it's drawn for
        bool presenting = false;
        bool new_frame = false;  // a capture not drawn before, not a redraw
        bool pace = false;       // published by PublishPacer
        int slot = -1;           // its output, on the zero-copy path
        bool drew_output = false, drew_gpu = false, want_rgba = false;
        GpuOutput out;
        GpuStats gs;
        RasterStats rs;
        // what its capture showed of the camera (cuts_)
        CameraCuts::Step camera;
        uint32_t width = 0, height = 0;
        bool done = true;
        // steady-clock nanoseconds: recorded and submitted, seen done, and
        // due (once done; -1 before)
        int64_t submitted_ns = 0, done_ns = 0, due_ns = -1;
    };

    void Run() {
        // whose stack a game stall's log samples (stall_watch.h)
        stall_watch::SetWorkerThread();
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
        // the presenting stretch the last frame was drawn for, and the window's
        // restores by then (SetPaused); and for the one starting, the
        // captures seen while waiting for its first
        uint64_t drawn_generation = 0, start_generation = 0, start_seen = 0;
        uint64_t drawn_resumes = 0, start_resumes = 0;
        uint32_t start_waited = 0;
        gpu_held_ = false;
        // The zero-copy path's frame submitted and not yet published, which
        // the GPU may still be drawing. One at most: with
        // native_present_pipeline the worker records the next capture into
        // another output while the GPU draws it, and publishes it before it
        // records another, so the GPU never has more than two of the
        // worker's frames to draw; without, it waits for each once
        // submitted, and the GPU has one.
        std::optional<Drawn> flight;
        // When `d`, drawn by `done_ns`, is to be published: a new frame for
        // the window a steady delay after the game presented it
        // (PublishPacer), a redraw (new options, a screenshot of a paused
        // game) at once. The worker publishes sooner once the next capture
        // is there or a setting or a user changes.
        auto due_of = [&](const Drawn& d, int64_t done_ns) {
            if (!d.pace || !d.new_frame) return done_ns;
            if (d.generation != pacer_generation) {
                pacer.Reset();
                pacer_generation = d.generation;
            }
            return pacer.Due(d.cap->frame, Nanoseconds(d.presented), done_ns);
        };
        while (true) {
            // for a game stall's log (stall_watch.h)
            stall_watch::SetWorker(stall_watch::Worker::kWaiting);
            RasterOptions o;
            std::string dump;
            bool changed, gpu, zero_copy, want_rgba, presenting, pace, pipeline;
            // the window minimized (SetPaused), and no screenshot asked for:
            // nothing to draw
            bool paused;
            // the presenting stretch this frame is drawn for (PresentSlots),
            // and the window's restores so far
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
                paused = presenting && pause_.Paused() && !image_wanted_;
                generation = slots_.Generation();
                resumes = resumes_;
                pace = presenting && REXCVAR_GET(native_present_pacing);
                // the next frame recorded while the GPU draws this one, or
                // each waited for once submitted
                pipeline = REXCVAR_GET(native_present_pipeline);
                o.gpu_no_wait = pipeline;
                // what's drawn once a world period is kept that long on the
                // GPU, not sent again each time (gpu_view.h's residency)
                o.world_period = pacing::WorldPeriod();
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
            // the next capture being a new frame anyway. The same once the
            // window is restored after a pause, whose kept post buffer is
            // from before it, the window showing the last frame meanwhile.
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
            // something to draw: a capture not drawn yet, or a setting or a
            // user changed, while the window isn't minimized
            const bool work = cap && fresh && (cap->frame != last_frame || changed) && !paused;
            int slot = -1;
            if (flight) {
                // The frame in flight is published once the GPU has finished
                // it and it's due, or as soon as it's finished once there's
                // something new to draw. Until it's finished, something new
                // is drawn meanwhile into another output, with
                // native_present_pipeline, if one is free and the GPU isn't
                // held (WaitFlight); else the worker waits for it, as nothing
                // else can go on.
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
                    // (a frame left in flight as native_present_pipeline
                    // turned off is waited for, never overlapped)
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
                // Minimized: until it's restored, a screenshot is asked for
                // or presenting stops, not woken by each capture the game
                // publishes. A setting changing meanwhile needs no redraw:
                // restored, the window waits for a new capture anyway.
                std::unique_lock lock(mutex_);
                paint_cv_.wait_for(lock, kIdleWait, [&] {
                    return stop_ || !present_ || !pause_.Paused() || image_wanted_;
                });
                epoch = CaptureEpoch();
                continue;
            }
            if (!work) {
                // until the game publishes a capture, or a setting or a user
                // changes (WakeCaptureWaiters)
                epoch = WaitForCapture(epoch, kIdleWait);
                continue;
            }
            // an output no paint may still be sampling, or wait for one
            if (zero_copy && slot < 0) {
                slot = AcquireSlot();
                if (slot < 0) {
                    std::lock_guard lock(mutex_);
                    options_changed_ |= changed;
                    continue;
                }
            }
            const bool new_frame = cap->frame != last_frame;
            last_frame = cap->frame;
            if (presenting) {
                drawn_generation = generation;
                drawn_resumes = resumes;
            }
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
            // RenderFrame fails (and stays failed) without a device, and the
            // CPU draws instead
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
                // native_present_pipeline off: the GPU finishes each frame
                // before the worker goes on, as it did before the pipeline
                if (d.drew_output && !pipeline) WaitFlight(d, nullptr);
                // an output that failed the zero-copy checks is read back, and
                // the drawer turns to uploading from the next paint; reading
                // it back waits for the GPU, so the frame is done at once
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
            if (flight && slot >= 0 && !d.drew_output) {
                // the GPU failed, and gpu_view let its device go, the frame
                // before's output with it: nobody's
                std::lock_guard lock(mutex_);
                slots_.Abandon(flight->slot);
                flight.reset();
            }
            if (flight) {
                // the frame before, which the GPU was still drawing as this
                // one was recorded: published as soon as it's finished, this
                // one being newer (and noted by the pacer, in order)
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
                // in flight until the GPU finishes it (above, next time
                // round), or stopping before a read back's frame finished
                if (!d.want_rgba) {
                    flight = std::move(d);
                } else {
                    std::lock_guard lock(mutex_);
                    slots_.Abandon(d.slot);
                }
                continue;
            }
            // drawn, and finished: published when due
            const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
            const int64_t due = due_of(d, now);
            if (due > now) WaitForCapture(seen, std::chrono::nanoseconds(due - now));
            PublishDrawn(d, d.want_rgba ? &rgba : nullptr);
        }
    }

    // One look at the fence of `f`, in flight: done, and its slot finished
    // (PresentSlots::Finished), once the GPU has finished it; and the worker
    // no longer held, if it was (WaitFlight).
    bool PollFlight(Drawn& f) {
        if (f.done) return true;
        if (!GpuRenderer::Get().OutputDone(f.slot)) return false;
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

    // Waits for the GPU to finish `f`, looking every kFencePoll, until it
    // has (true), the worker stops, or with `seen` a capture or a setting
    // moves CaptureEpoch on from it (something new to draw). The time
    // waited is added to the frame's wait_ms. A frame the GPU hasn't
    // finished kGpuHung after it was submitted holds the worker: it submits
    // no more frames (the window keeps the last one published) until the
    // GPU finishes it, rather than pile them onto a GPU that may have hung.
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

    // `d` to whoever wants it: its output to the window (if still the
    // stretch it was drawn for), its numbers to the live view, its RGBA
    // (`rgba`, if it has one) to F9's window, a screenshot and the upload
    // path, and its description to F9's window. A slot's frame must be done.
    void PublishDrawn(const Drawn& d, const std::vector<uint32_t>* rgba) {
        const std::string stats =
            Describe(*d.cap, d.drew_gpu ? DescribeGpu(d.gs, d.slot >= 0) : DescribeRaster(d.rs));
        // native_slow_frame_ms: a new frame the GPU took too long over, and
        // the captures the worker skipped before it
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
        // Never a frame the GPU may still be drawing: Publish would refuse
        // it, and abandoning it as refused would free a slot the GPU is
        // writing for the next frame to take. Every caller has waited for it
        // (PollFlight); one that hasn't leaves it as it is, taken.
        if (d.slot >= 0 && (!d.done || slots_.Unfinished(d.slot))) {
            if (!unfinished_logged_) {
                unfinished_logged_ = true;
                REXLOG_WARN("native renderer: a frame was to be published before the GPU had "
                            "finished it; left unpublished");
            }
            return;
        }
        // still the stretch it was drawn for (PresentSlots::Publish)
        const bool current = d.presenting && d.generation == slots_.Generation();
        // told once the lock is let go, for a frame the window can now show
        // (SetPublished)
        std::shared_ptr<const std::function<void()>> published;
        // the window has the frame now, by either path
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
        // a capture from before the numbers started over (one left from
        // the last time, or redrawn for new options) isn't counted
        if (live_measuring_ && d.cap->frame > live_last_) {
            const uint64_t skipped = d.cap->frame - live_last_ - 1;
            live_.skipped_busy += skipped;
            live_last_ = d.cap->frame;
            live_.rendered++;
            if (ProcKnown(*d.cap) && !DrawsWorld(*d.cap) && !d.cap->composed) live_.worldless++;
            const double ms = d.drew_gpu ? d.gs.ms : d.rs.ms;
            live_.ms.push_back(ms);
            if (d.drew_gpu) live_.wait_ms.push_back(d.gs.wait_ms);
            // by kind: the captures skipped are the frame before's, which
            // they came in while it was drawn (this one's for the first)
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
            // a screenshot asked for while presenting is of a frame
            // drawn for the window's stretch (Take's `for_present`)
            if (current || !present_) image_wanted_ = false;
            // the upload path's picture for the window
            if (current && d.slot < 0) {
                published = published_;
                to_window = true;
            }
        }
        stats_ = stats;
        lock.unlock();
        // a frame drawn again (new options) isn't a new one to time
        if (to_window && d.new_frame) NotePublished(d.presented);
        if (published) (*published)();
    }

    // native_slow_frame_ms's line for `d`, kSlowPerSecond a second at most;
    // the next logged says how many were left out
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
    // the window minimized, with emulated_gpu off (SetPaused), and for how
    // long; and the times it was restored, for the worker's first frame after
    DrawPause pause_;
    uint64_t resumes_ = 0;
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
    std::shared_ptr<const std::function<void()>> published_;
    // the worker's alone: a frame has been in flight kGpuHung, and it
    // submits none until the GPU finishes it (WaitFlight)
    bool gpu_held_ = false;
    // PublishDrawn was handed an unfinished frame (logged once)
    bool unfinished_logged_ = false;
    // the live view's numbers; captures numbered up to live_base_ came before
    // they started, and live_last_ is the newest one counted
    bool live_measuring_ = false;
    LiveViewStats live_;
    uint64_t live_base_ = 0, live_last_ = 0;
    // the newest counted frame's FrameKind, -1 none yet
    int live_last_kind_ = -1;
    std::optional<bool> live_drew_gpu_;
    // the worker's alone: the newest capture published, and the slow frames
    // logged in the second from slow_since_ns_ and left out since the last
    // logged (LogSlow)
    uint64_t published_frame_ = 0;
    int64_t slow_since_ns_ = 0;
    uint32_t slow_logged_ = 0, slow_left_out_ = 0;
    // the worker's alone: the camera's cuts over the frames it draws
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

void NotePublished(std::chrono::steady_clock::time_point presented) {
    const int64_t now = Nanoseconds(std::chrono::steady_clock::now());
    std::lock_guard lock(g_paints_mutex);
    g_paints.Published(now, presented.time_since_epoch().count() ? Nanoseconds(presented) : 0);
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
            if (v != "native" && sync_gpu::NativeOnly())
                REXLOG_INFO("renderer {}: {}", v, sync_gpu::kNoEmulatedGpuSwitch);
            Follow(v == "native");
        });
        Follow(REXCVAR_GET(renderer) == "native");
        // With emulated_gpu off no emulated swap asks the window to paint, so
        // each frame published for it does (on the UI thread, where the
        // presenter takes the request); a request already on its way covers
        // the frames published before it runs. None while the window can't be
        // seen (PaintTarget::Seen): it kept painting each frame minimized.
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
            REXLOG_INFO("native present: asking for a paint with each frame (emulated_gpu off)");
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

    // On the UI thread, with emulated_gpu off: the worker draws nothing
    // while the window is minimized (Renderer::SetPaused; with the emulated
    // GPU on it draws on, as the emulated GPU does). And the paint requests,
    // which a restored window has had none of while it was minimized: one at
    // once (a frame published next would ask too, a frame later).
    void SetMinimized(bool minimized) {
        if (sync_gpu::NativeOnly()) Renderer::Get().SetPaused(minimized);
        if (!paint_target_) return;
        paint_target_->minimized = minimized;
        if (!minimized && paint_target_->window && paint_target_->Seen())
            paint_target_->window->RequestPresenterUIPaintFromUIThread();
    }

    // renderer, as it changes: the worker starts when it turns native, on the
    // UI thread where the GPU device starts, and is let go when it turns
    // emulated, once the emulated GPU's picture is the game's again: while
    // native it skipped the game's draws (gpu_skip.h), so it keeps drawing
    // the window until the emulated GPU has swapped whole frames enough
    // (SkipLatch::Fresh: two, and with even/odd rendering a post frame after
    // a whole world frame; gpu_skip.cpp calls back then), or kMaxDrain has
    // gone by
    // (`at_once`: at shutdown, without waiting). With emulated_gpu off it's
    // the only picture: native whatever renderer says, until shutdown.
    void Follow(bool native, bool at_once = false) {
        if (!at_once && sync_gpu::NativeOnly()) native = true;
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
    // GpuSkipStats::passes_dropped when it last turned native
    uint64_t passes_dropped_on_ = 0;
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
    // emulated_gpu off: the window a published frame asks to paint (read and
    // cleared on the UI thread), and whether a request is on its way (Start)
    struct PaintTarget {
        rex::ui::Window* window = nullptr;
        std::atomic<bool> pending{false};
        // the UI thread's alone: the SDK's word (NativePresentMinimized), and
        // whether the last request was held back, to log the change
        bool minimized = false;
        bool held = false;

        // whether a paint would be seen (PaintWanted): the system's word
        // where band3 can ask it (Windows), the SDK's elsewhere
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

void NativePresentMinimized(bool minimized) {
    if (g_present) g_present->SetMinimized(minimized);
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
