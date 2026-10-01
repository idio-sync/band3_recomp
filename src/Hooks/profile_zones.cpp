#include <rex/dbg.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"

// Tracy zones around RB3's top-level engine functions, so a capture shows how
// each frame splits between game logic, the players and tracks, characters,
// the venue and crowd, the UI, audio and each stage of drawing. Profiling is
// only compiled into non-Release builds (the SDK's REXGLUE_ENABLE_PROFILING), so
// Release builds leave these functions alone. Pair with the autoplay setting for
// runs that can be compared.

#ifdef REXGLUE_ENABLE_PROFILING

#define BAND3_PROFILE_ZONE(function, label)                                    \
    extern "C" void __imp__##function(PPCContext& ctx, uint8_t* base);         \
    extern "C" REX_FUNC(function)                                              \
    {                                                                          \
        SCOPE_profile_cpu_f(label);                                            \
        __imp__##function(ctx, base);                                          \
    }

// the frame (App::DrawRegular's zone is in frame_counter.cpp, which hooks it in
// every build)
BAND3_PROFILE_ZONE(TaskMgr__Poll, "RB3 TaskMgr::Poll")

// game logic and players
BAND3_PROFILE_ZONE(Game__Poll, "RB3 Game::Poll")
BAND3_PROFILE_ZONE(GemPlayer__Poll, "RB3 GemPlayer::Poll")
BAND3_PROFILE_ZONE(VocalPlayer__Poll, "RB3 VocalPlayer::Poll")
BAND3_PROFILE_ZONE(BandDirector__Poll, "RB3 BandDirector::Poll")

// tracks
BAND3_PROFILE_ZONE(TrackDir__Poll, "RB3 TrackDir::Poll")
BAND3_PROFILE_ZONE(TrackDir__DrawShowing, "RB3 TrackDir::DrawShowing")
BAND3_PROFILE_ZONE(GemTrackDir__Poll, "RB3 GemTrackDir::Poll")

// characters
BAND3_PROFILE_ZONE(BandCharacter__Poll, "RB3 BandCharacter::Poll")
BAND3_PROFILE_ZONE(BandCharacter__DrawShowing, "RB3 BandCharacter::DrawShowing")
BAND3_PROFILE_ZONE(Character__DrawShowing, "RB3 Character::DrawShowing")
BAND3_PROFILE_ZONE(CharDriver__Poll, "RB3 CharDriver::Poll")

// venue and crowd
BAND3_PROFILE_ZONE(WorldDir__Poll, "RB3 WorldDir::Poll")
BAND3_PROFILE_ZONE(WorldDir__DrawShowing, "RB3 WorldDir::DrawShowing")
BAND3_PROFILE_ZONE(WorldCrowd__Poll, "RB3 WorldCrowd::Poll")
BAND3_PROFILE_ZONE(WorldCrowd__DrawShowing, "RB3 WorldCrowd::DrawShowing")

// UI and audio
BAND3_PROFILE_ZONE(UIManager__Poll, "RB3 UIManager::Poll")
BAND3_PROFILE_ZONE(UIManager__Draw, "RB3 UIManager::Draw")
BAND3_PROFILE_ZONE(Synth360__Poll, "RB3 Synth360::Poll")
BAND3_PROFILE_ZONE(MasterAudio__FillSwing, "RB3 MasterAudio::FillSwing")
BAND3_PROFILE_ZONE(SongPreview__Poll, "RB3 SongPreview::Poll")
// the singer's audio, per chunk the mics hand over
BAND3_PROFILE_ZONE(VoiceBeat__Analyze, "RB3 VoiceBeat::Analyze")

// loading and the network, where the menus hitch
BAND3_PROFILE_ZONE(CDRead, "RB3 CDRead")
BAND3_PROFILE_ZONE(HDCache__OpenFiles, "RB3 HDCache::OpenFiles")
BAND3_PROFILE_ZONE(CacheXbox__ThreadGetFileSize, "RB3 CacheXbox::ThreadGetFileSize")
BAND3_PROFILE_ZONE(NetCacheMgr__Poll, "RB3 NetCacheMgr::Poll")
BAND3_PROFILE_ZONE(HttpGet__Poll, "RB3 HttpGet::Poll")

// drawing stages
BAND3_PROFILE_ZONE(DxRnd__BeginDrawing, "RB3 DxRnd::BeginDrawing")
BAND3_PROFILE_ZONE(DxRnd__DoWorldEnd, "RB3 DxRnd::DoWorldEnd")
BAND3_PROFILE_ZONE(DxRnd__DoPostProcess, "RB3 DxRnd::DoPostProcess")
BAND3_PROFILE_ZONE(RndPostProc__DoPost, "RB3 RndPostProc::DoPost")
// DxRnd::Present is zoned in src/Render/scene_capture.cpp, which hooks it

#endif
