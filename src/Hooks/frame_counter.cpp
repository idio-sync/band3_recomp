#include <rex/dbg.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <bit>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/Hooks/keyboard_search.h"
#include "src/Hooks/mouse_hover.h"
#include "src/Net/http_server.h"
#include "src/Test/game_state.h"
#include "src/Test/test_server.h"

// Counts the frames RB3 draws, for the test harness's `wait frames=N`, keeps
// the song's position for the web server's /status, runs the game work the
// web server's requests wait on (src/Net/http_server.h), and sends the game
// the keys typed into Deluxe's search (keyboard_search.h), and hovers the
// mouse over the focused list's rows (mouse_hover.h).
// App::DrawRegular runs once per frame; profiling builds also give it its zone
// here, next to the others in profile_zones.cpp.

namespace {

// The song's clock: TaskMgr::Seconds(TaskMgr*, TimeReference) reads its
// timeline's seconds (one reference less a few milliseconds of latency). There's
// no timeline while the song loads, and it's below zero before the song starts.
constexpr uint32_t kTheTaskMgr = 0x82E051A0;     // TaskMgr object
constexpr uint32_t kTaskMgr_Timeline = 40;       // its timeline*
constexpr uint32_t kTimeline_Seconds = 16;       // float

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

float LoadFloat(uint8_t* base, uint32_t addr) {
    return std::bit_cast<float>(Load32(base, addr));
}

void RecordSongTime(uint8_t* base) {
    auto& state = band3::test::GameState::Get();
    if (!state.InGame()) return;
    const uint32_t timeline = Load32(base, kTheTaskMgr + kTaskMgr_Timeline);
    if (!timeline) return;
    const float seconds = LoadFloat(base, timeline + kTimeline_Seconds);
    state.SetSongTime(seconds >= 0.0f ? static_cast<int32_t>(seconds * 1000.0f) : -1);
}

}

extern "C" void __imp__App__DrawRegular(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(App__DrawRegular)
{
#ifdef REXGLUE_ENABLE_PROFILING
    SCOPE_profile_cpu_f("RB3 App::DrawRegular");
#endif
    band3::test::GameState::Get().CountFrame();
    if (band3::test::Enabled() || band3::http::Enabled()) RecordSongTime(base);
    band3::http::RunGameJobs(ctx, base);
    band3::keyboard_search::RunFrame(ctx, base);
    band3::mouse_hover::RunFrame(ctx, base);
    __imp__App__DrawRegular(ctx, base);
}
