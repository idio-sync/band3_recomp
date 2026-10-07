#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/types.h>
#include <rex/ui/windowed_app_context.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include "generated/band3_init.h"
#include "src/Hooks/song_pause.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/menu_shortcut.h"
#include "src/Input/mouse_menus_driver.h"
#include "src/Input/ui_round_trip.h"

// Serializes the game's calls into the SDK's input system (see input_lock.h).
// These are the only guest functions that call XamInputGetState,
// XamInputGetCapabilities or XamInputSetState: the XInput library's wrappers.
// Each player's state and type, as the game reads them, also go to the menu
// shortcut (menu_shortcut.h), and each read times the UI thread under the
// test harness (ui_round_trip.h).

namespace band3::input {

std::recursive_mutex& InputLock() {
    static std::recursive_mutex lock;
    return lock;
}

// The UI thread's round trips (ui_round_trip.h): one request on its way at a
// time, as the SDL driver's pump is, so it waits where the pump would
namespace {
std::atomic<rex::ui::WindowedAppContext*> g_probe_context{nullptr};
std::atomic<bool> g_probe_in_flight{false};
std::mutex g_probe_mutex;
UiRoundTripHistogram g_probe_trips;
}

void StartUiRoundTripProbe(rex::ui::WindowedAppContext* app_context) {
    g_probe_context.store(app_context);
}

void StopUiRoundTripProbe() { g_probe_context.store(nullptr); }

void ProbeUiRoundTrip() {
    rex::ui::WindowedAppContext* context = g_probe_context.load(std::memory_order_acquire);
    if (!context || g_probe_in_flight.load(std::memory_order_relaxed) ||
        g_probe_in_flight.exchange(true, std::memory_order_acquire))
        return;
    const auto sent = std::chrono::steady_clock::now();
    const bool queued = context->CallInUIThread([sent] {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent)
                .count();
        {
            std::lock_guard<std::mutex> lock(g_probe_mutex);
            g_probe_trips.Add(ms);
        }
        g_probe_in_flight.store(false, std::memory_order_release);
    });
    if (!queued) g_probe_in_flight.store(false, std::memory_order_release);
}

UiRoundTripStats GetUiRoundTripStats(bool reset) {
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    UiRoundTripStats out;
    out.runs = g_probe_trips.Count();
    out.p50 = g_probe_trips.Percentile(0.5);
    out.p95 = g_probe_trips.Percentile(0.95);
    out.max = g_probe_trips.Max();
    if (reset) g_probe_trips.Reset();
    return out;
}

}

#define BAND3_INPUT_LOCKED(function)                                           \
    extern "C" void __imp__##function(PPCContext& ctx, uint8_t* base);         \
    extern "C" REX_FUNC(function)                                              \
    {                                                                          \
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock()); \
        __imp__##function(ctx, base);                                          \
    }

// XInputGetState(user, state): XamInputGetState(user, 1, state). The game's
// joypad loop (RunXinputJoypadLoop) reads every player through it each pass.
// The mouse's menu presses join the lowest connected player's here
// (mouse_menus_driver.h), and the pause menu's Start the player it's for
// (song_pause.h).
extern "C" void __imp__rex_sub_8283FB80(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(rex_sub_8283FB80) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t state = ctx.r4.u32;
    {
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock());
        __imp__rex_sub_8283FB80(ctx, base);
    }
    rex::input::X_INPUT_STATE* read = nullptr;
    if (ctx.r3.u32 == 0 && state) {
        read = reinterpret_cast<rex::input::X_INPUT_STATE*>(base + state);
    }
    band3::input::AddMouseMenuPresses(user, read);
    band3::song_pause::AddPress(user, read, base);
    std::optional<uint16_t> buttons;
    if (read) buttons = read->gamepad.buttons;
    band3::input::GameChordPads().OnState(user, buttons, band3::input::ChordPads::Clock::now());
    band3::input::ProbeUiRoundTrip();
}

// XInputGetCapabilities(user, flags, caps): XamInputGetCapabilities. The game
// reads a player's type through it when the player connects, and keeps it
// (JoypadGetCachedXInputCaps). A guitar's type is the one guitar_type says
// (input_system.h), which decides whether RB3 reads its left trigger as the
// effect switch.
extern "C" void __imp__sub_8283FB78(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8283FB78) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t caps = ctx.r5.u32;
    {
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock());
        __imp__sub_8283FB78(ctx, base);
    }
    if (ctx.r3.u32 == 0 && caps) {
        auto* read = reinterpret_cast<rex::input::X_INPUT_CAPABILITIES*>(base + caps);
        read->sub_type = band3::input::GameGuitarSubtype(user, read->sub_type);
        band3::input::GameChordPads().OnCapabilities(user, read->sub_type);
    }
}

// XInput2's device capabilities read
BAND3_INPUT_LOCKED(rex_sub_8284E1D8)
// XInput2's poll, which takes the library's own critical section and calls
// XamInputGetState and the capabilities read above
BAND3_INPUT_LOCKED(XInput2Sample)
// XamInputGetCapabilities, then XamInputSetState (vibration)
BAND3_INPUT_LOCKED(rb3_XInputSetState)
