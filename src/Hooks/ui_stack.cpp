#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>

// A push that arrives while a pop is still pending. UIManager::PopScreen only
// marks the pop (kTransitionPop) and UIManager::Poll finishes it later, so a
// script that pops a screen and pushes another in the same frame (Rock Band 3
// Deluxe's first-run welcome: pop dx_welcome_screen, push
// hint_rb3_welcome_screen) reaches PushScreen with the pop pending.
// PushScreen's CancelTransition drops the pop, and it then pushes the screen
// that was being popped: the stack keeps it, and the screen under it stays
// pushed for good. With anything pushed, BandScreen::LoadPanels skips
// LoadInterstitials, so the transition vignettes keep the main menu's table,
// and the taxi vignette into Customize Character sends the game back into
// itself forever.
//
// Here the push replaces the screen being popped instead: GotoScreenImpl to
// the new screen, which cancels the pop just as PushScreen's own
// CancelTransition does, without pushing the popped screen. The stack is then
// what pop-then-push means, the new screen over the one the pop returned to.
// Pushes with no pop pending are untouched. (rb3-xenon ui/UI.cpp for the
// retail functions.)

extern "C" void __imp__UIManager__PushScreen(PPCContext& ctx, uint8_t* base);
REX_EXTERN(UIManager__GotoScreenImpl);

namespace {

constexpr uint32_t kUIManager_TransitionState = 0x10;
constexpr uint32_t kUIManager_CurrentScreen = 0x2c;
constexpr uint32_t kUIScreen_Name = 0x18;
constexpr uint32_t kTransitionPop = 3;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

const char* ScreenName(uint8_t* base, uint32_t screen) {
    const uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
    return name ? rex::memory::GuestPtr<const char*>(base, name) : "(none)";
}

}

// UIManager::PushScreen(UIManager*, UIScreen*)
extern "C" REX_FUNC(UIManager__PushScreen)
{
    const uint32_t ui = ctx.r3.u32;
    const uint32_t screen = ctx.r4.u32;
    if (!ui || !screen || Load32(base, ui + kUIManager_TransitionState) != kTransitionPop) {
        __imp__UIManager__PushScreen(ctx, base);
        return;
    }
    const uint32_t popping = Load32(base, ui + kUIManager_CurrentScreen);
    REXLOG_INFO("UI: {} is pushed while {} is being popped; it replaces that screen",
                ScreenName(base, screen), ScreenName(base, popping));

    // UIManager::GotoScreenImpl(UIManager*, UIScreen*, bool force, bool back)
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    call.r3.u64 = ui;
    call.r4.u64 = screen;
    call.r5.u64 = 0;
    call.r6.u64 = 0;
    UIManager__GotoScreenImpl(call, base);
}
