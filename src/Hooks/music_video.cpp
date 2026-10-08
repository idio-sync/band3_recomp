// Music videos: replaces the frame a venue's Bink movie is about to draw with
// the song's music video (src/Video/music_video.h), or with a test pattern,
// and with music_video_hide_band, leaves the band undrawn while it shows. The
// Black Background modifier with black_background_lights is a video venue
// drawn the same way: black, no band, its lights on. With music_video_as_is,
// the venue's post-processing is left off meanwhile, so the video shows as it
// is.
// Movie::Impl::Draw (rb3-xenon movie/Movie.cpp) gives its material the
// current frame's three plane textures and draws them; the planes are
// written here first, so both renderers draw ours without knowing. The video
// venues draw theirs into a texture (clip.tex) that fills the screen behind
// the band.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include "generated/band3_init.h"
#include "src/Hooks/music_video.h"
#include "src/Render/guest_formats.h"
#include "src/Video/movie_planes.h"
#include "src/Video/music_video.h"
#include "src/settings.h"

namespace {

namespace gf = band3::render::guest_format;
using band3::video::PlaneSet;

// Movie::Impl (movie/Movie_Xbox.h)
constexpr uint32_t kImpl_FilenameStr = 0x8 + 0x8;  // String mFilename's char*
constexpr uint32_t kImpl_Bink = 0x14;
constexpr uint32_t kImpl_Buffers = 0xdc;
// MovieInternalBuffers: RndTex* mTex[3][2][2] (plane, frame, half), then
// BINKFRAMEBUFFERS at 0x34 (TotalFrames, ..., FrameNum at 0x14), mNextFrame;
// Movie::Impl::Draw draws frame FrameNum, half mNextFrame >= TotalFrames
constexpr uint32_t kBufs_TotalFrames = 0x34;
constexpr uint32_t kBufs_FrameNum = 0x34 + 0x14;
constexpr uint32_t kBufs_NextFrame = 0xb0;
// RndTex, DxTex and the XDK texture (scene_capture.cpp's)
constexpr uint32_t kTex_Type = 0x48;
constexpr uint32_t kTexType_Movie = 4;
constexpr uint32_t kDxTex_Texture = 0x78;
constexpr uint32_t kD3DBaseTexture_Fetch = 0x1c;
// the song's clock, as frame_counter.cpp reads it: TaskMgr's timeline's
// seconds; no timeline while the song loads
constexpr uint32_t kTheTaskMgr = 0x82E051A0;
constexpr uint32_t kTaskMgr_Timeline = 40;
constexpr uint32_t kTimeline_Seconds = 16;
// the video venues' movies, which are the ones replaced
constexpr char kVenueMovies[] = "world/venue/video/";

// whether the movie's file is one of a video venue's
bool VenueMovie(uint8_t* base, uint32_t impl) {
    const uint32_t str = REX_LOAD_U32(impl + kImpl_FilenameStr);
    if (!str) return false;
    for (uint32_t i = 0; kVenueMovies[i]; i++) {
        if (char(REX_LOAD_U8(str + i)) != kVenueMovies[i]) return false;
    }
    return true;
}

// a plane texture's fetch constant, or false if it isn't a movie's
bool PlaneFetch(uint8_t* base, uint32_t tex, uint32_t f[6]) {
    if (!tex || !(REX_LOAD_U32(tex + kTex_Type) & kTexType_Movie)) return false;
    const uint32_t d3d = REX_LOAD_U32(tex + kDxTex_Texture);
    if (!d3d) return false;
    for (int i = 0; i < 6; i++) f[i] = REX_LOAD_U32(d3d + kD3DBaseTexture_Fetch + i * 4);
    return true;
}

// Host memory for a fetch's base address, through a CPU view as Bink's own
// writes go, so the emulated GPU's page watches see ours too. The address is
// mostly a CPU view's already (the planes' are 0xE0000000+, whose view sits
// 0x1000 further into physical memory: REX_RAW_ADDR knows); a physical one
// goes through the 0xA0000000 view, which doesn't. Null for anything else,
// which a fetch's base can't be.
uint8_t* PlaneMemory(uint8_t* base, uint32_t address) {
    if (address < 0xA0000000u && address >= 0x20000000u) return nullptr;
    return REX_RAW_ADDR(address >= 0xA0000000u ? address : 0xA0000000u | address);
}

// the song's time in seconds, far before any video without a timeline
double SongTime(uint8_t* base) {
    const uint32_t timeline = REX_LOAD_U32(kTheTaskMgr + kTaskMgr_Timeline);
    if (!timeline) return -1000.0;
    return std::bit_cast<float>(REX_LOAD_U32(timeline + kTimeline_Seconds));
}

// the layouts of the planes the movie draws this frame, or false
bool DrawnPlanes(uint8_t* base, uint32_t impl, gf::FetchLayout l[3]) {
    const uint32_t bufs = REX_LOAD_U32(impl + kImpl_Buffers);
    if (!REX_LOAD_U32(impl + kImpl_Bink) || !bufs) return false;
    const uint32_t total = REX_LOAD_U32(bufs + kBufs_TotalFrames);
    const uint32_t frame = REX_LOAD_U32(bufs + kBufs_FrameNum);
    const uint32_t half = REX_LOAD_U32(bufs + kBufs_NextFrame) >= total ? 1 : 0;
    if (frame > 1) return false;
    for (uint32_t plane = 0; plane < 3; plane++) {
        uint32_t f[6];
        const uint32_t tex = REX_LOAD_U32(bufs + ((plane * 2 + frame) * 2 + half) * 4);
        if (!PlaneFetch(base, tex, f)) return false;
        l[plane] = gf::ReadFetchLayout(f);
        if (!l[plane].base_address) return false;
    }
    // each venue's planes, once
    static uint32_t logged = 0;
    if (logged != bufs) {
        logged = bufs;
        REXLOG_INFO("Venue movie planes: Y {}x{} ({}, pitch {}), chroma {}x{}", l[0].width,
                    l[0].height, l[0].tiled ? "tiled" : "linear", l[0].pitch_texels, l[1].width,
                    l[1].height);
    }
    return true;
}

// steady_clock ticks of the last black frame written, so the band stays
// hidden only while a black venue is drawing (not in the menus after it)
std::atomic<std::chrono::steady_clock::rep> g_black_drawn{0};

bool BlackShowing() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto half_second =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(500))
            .count();
    return now - g_black_drawn.load() < half_second;
}

std::shared_ptr<const PlaneSet> BlackPlanes(const band3::video::PlaneSizes& sizes) {
    static std::shared_ptr<PlaneSet> black;
    static band3::video::PlaneSizes made;
    if (!black || made != sizes) {
        black = std::make_shared<PlaneSet>();
        black->y.Resize(sizes.w, sizes.h, band3::video::kBlackY);
        black->cr.Resize(sizes.cw, sizes.ch, band3::video::kNeutralC);
        black->cb.Resize(sizes.cw, sizes.ch, band3::video::kNeutralC);
        made = sizes;
    }
    return black;
}

void ReplaceFrame(uint8_t* base, uint32_t impl) {
    const bool pattern = REXCVAR_GET(music_video_test_pattern);
    const bool black = band3::video::BlackVenue();
    if (!pattern && !black && !band3::video::MusicVideosOn()) return;
    if (!VenueMovie(base, impl)) return;
    gf::FetchLayout l[3];
    if (!DrawnPlanes(base, impl, l)) return;
    const band3::video::PlaneSizes sizes{l[0].width, l[0].height, l[1].width, l[1].height};
    std::shared_ptr<const PlaneSet> planes;
    if (pattern) {
        static const auto start = std::chrono::steady_clock::now();
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        auto made = std::make_shared<PlaneSet>();
        band3::video::TestPattern(*made, sizes.w, sizes.h, sizes.cw, sizes.ch, t);
        planes = std::move(made);
    } else if (black) {
        planes = BlackPlanes(sizes);
        g_black_drawn = std::chrono::steady_clock::now().time_since_epoch().count();
    } else if (!band3::video::MusicVideoFrame(SongTime(base), sizes, planes)) {
        return;
    }
    uint8_t* memory[3];
    for (int i = 0; i < 3; i++) {
        memory[i] = PlaneMemory(base, l[i].base_address);
        if (!memory[i]) return;
    }
    const band3::video::Plane* src[3] = {&planes->y, &planes->cr, &planes->cb};
    for (int i = 0; i < 3; i++) band3::video::WritePlane8(l[i], memory[i], *src[i]);
}

}

extern "C" void __imp__Movie__Impl__Draw(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(Movie__Impl__Draw) {
    ReplaceFrame(base, ctx.r3.u32);
    __imp__Movie__Impl__Draw(ctx, base);
}

// music_video_hide_band: a band member draws through
// BandCharacter::DrawLodOrShadow (rb3-xenon bandobj/BandCharacter.cpp), its
// outfit and its instrument, and its shadows; skipped while a music video
// shows, or a black venue. It still animates, and the cameras and lights go
// on as they were.
extern "C" void __imp__BandCharacter__DrawLodOrShadow(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(BandCharacter__DrawLodOrShadow) {
    if (REXCVAR_GET(music_video_hide_band) && band3::video::MusicVideoShowing()) return;
    if (BlackShowing()) return;
    __imp__BandCharacter__DrawLodOrShadow(ctx, base);
}

namespace {

// TheRnd (scene_capture.cpp's kDrawModeHolder) and its mDisablePostProc,
// which Rnd::DoPostProcess skips every post-processor for (the game's own
// toggle_all_postprocs) and the native renderer reads (ReadPostParams): the
// video venues' colour filter, film grain, glow and trails
constexpr uint32_t kTheRnd = 0x82C76B68;
constexpr uint32_t kRnd_DisablePostProc = 0x105;

// whether band3 turned it on, so it's turned off only then
bool g_post_off = false;

}

namespace band3::music_video {

void RunFrame(uint8_t* base) {
    const bool want = REXCVAR_GET(music_video_as_is) &&
                      (band3::video::MusicVideoShowing() || BlackShowing());
    if (want == g_post_off) return;
    const uint32_t rnd = REX_LOAD_U32(kTheRnd);
    if (!rnd) return;
    REX_STORE_U8(rnd + kRnd_DisablePostProc, want ? 1 : 0);
    g_post_off = want;
}

}
