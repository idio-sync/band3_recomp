#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include "generated/band3_init.h"
#include "src/Game/DataArray.h"
#include "src/Game/Script.h"

// A Quit button on the main menu, under its bottom button (Play a Show on
// Rock Band 3 Deluxe, Music Store on the game as it shipped), with a screen
// that asks before quitting. Deluxe makes the same button for band3 when its
// scripts see MHX_PC, which band3 defines (config.cpp): recomp_extra_button
// and confirm_exit_game_screen, ui/main/main_hub.dta and
// dx/ui/dx_ui_screens.dta in its repo, since its 2026-03-05 commits. Older
// Deluxe builds and the game as it shipped have neither, so this makes them
// the same way under the same names; where Deluxe has made them, it keeps its
// own. The confirm screen's Quit calls {exit}, which band3 answers by closing
// the game (DTAFunctions.cpp).
//
// The scripts run as RockCentralGateway::ExecuteConfig parses them, without
// the game's DTA macros, so SELECT_MSG, kCopyShallow and the rest are spelled
// out. rb3-xenon, src/band3/meta_band/MainHubPanel.cpp for the panel.

extern "C" void __imp__MainHubPanel__Enter(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MainHubPanel__UpdateStateView(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MainHubPanel__Handle_82622F20(PPCContext& ctx, uint8_t* base);
REX_EXTERN(DataSetThis);

namespace {

// MainHubPanel::MainHubState
constexpr uint32_t kMainHubState_Main = 1;
// Hmx::Object::mName
constexpr uint32_t kObject_Name = 0x18;

// the confirm screen, on the welcome hint's panel (its two buttons and text),
// in the main ObjectDir. B goes back (kAction_Cancel is 2).
constexpr const char* kConfirmScreen = R"({if {! {exists confirm_exit_game_screen}}
   {new UIPanel band3_quit_panel
      (file "ui/hints/hint_rb3_welcome.milo")
      (focus "customize.btn")
      (enter
         {$this set_focus customize.btn}
         {description.lbl set edit_text "Are you sure you want to quit Rock Band 3?"}
         {customize.btn set edit_text "Quit"}
         {continue.btn set edit_text "Keep Rockin'"})
      (component_select ($component $user)
         {switch $component
            (customize.btn {exit})
            (continue.btn {ui pop_screen})
            kDataUnhandled})
      (button_down ($user $raw_button $action $pad_num)
         {if_else {== $action 2}
            {do {ui pop_screen} 1}
            kDataUnhandled}))
   {new BandScreen confirm_exit_game_screen
      (panels band3_quit_panel)
      (focus band3_quit_panel)}})";

// the button, a copy of mb_musicstore.btn 43 units below it in a copy of its
// group, as Deluxe's recomp_extra_button makes it; shown on the top menu only
constexpr const char* kButton = R"({with main_hub_panel
   {if {&& {! {exists mb_exit.btn}} {exists mb_musicstore.btn}}
      {new BandButton mb_exit.btn}
      {mb_exit.btn copy mb_musicstore.btn 1}
      {new Group mb_exit.grp}
      {mb_exit.grp copy mb_musicstore.grp 1}
      {mb_exit.grp add_object mb_exit.btn}
      {mb_exit.btn set_trans_parent mb_exit.grp}
      {mb_exit.grp remove_object mb_musicstore.btn}
      {mb_exit.btn set_local_pos_index 2 -43}
      {mb_exit.btn set edit_text "Quit"}
      {mb_musicstore.btn set nav_down mb_exit.btn}
      {menu_buttons.grp add_object mb_exit.grp}}
   {if {exists mb_exit.btn}
      {mb_exit.btn set_showing {== {$this get_state} 1}}}})";

PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

// runs the script with no $this, so names are found in the main ObjectDir
// (where main_hub_panel is), then puts $this back
void RunInMainDir(PPCContext& ctx, uint8_t* base, const char* script) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = 0;
    DataSetThis(call, base);
    const uint32_t old_this = call.r3.u32;

    band3::RunScript(ctx, base, script);

    call = CallContext(ctx);
    call.r3.u64 = old_this;
    DataSetThis(call, base);
}

const char* GuestString(uint8_t* base, uint32_t addr) {
    return addr ? reinterpret_cast<const char*>(REX_RAW_ADDR(addr)) : nullptr;
}

// whether msg is (component_select <mb_exit.btn> <user>)
bool IsQuitSelect(uint8_t* base, uint32_t msg) {
    auto* array = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(msg));
    if (static_cast<short>(array->mSize) < 3) return false;
    auto* nodes = reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(array->mNodes));
    if (nodes[1].type != band3::kDataSymbol || nodes[2].type != band3::kDataObject) return false;
    const char* type = GuestString(base, nodes[1].value);
    if (!type || std::strcmp(type, "component_select") != 0) return false;
    const uint32_t component = nodes[2].value;
    const char* name = component ? GuestString(base, REX_LOAD_U32(component + kObject_Name)) : nullptr;
    return name && std::strcmp(name, "mb_exit.btn") == 0;
}

}  // namespace

// MainHubPanel::Enter(MainHubPanel*)
extern "C" REX_FUNC(MainHubPanel__Enter) {
    __imp__MainHubPanel__Enter(ctx, base);

    static bool made_screen = false;
    if (!made_screen) {
        made_screen = true;
        RunInMainDir(ctx, base, kConfirmScreen);
    }
    RunInMainDir(ctx, base, kButton);
}

// MainHubPanel::UpdateStateView(this, MainHubState new, MainHubState old,
// MainHubOverride new, MainHubOverride old): Quit shows on the top menu only,
// as Deluxe's poll has it, since the submenus' buttons take its place
extern "C" REX_FUNC(MainHubPanel__UpdateStateView) {
    const uint32_t state = ctx.r4.u32;
    PPCContext after = ctx;
    __imp__MainHubPanel__UpdateStateView(ctx, base);
    RunInMainDir(after, base,
                 state == kMainHubState_Main
                     ? "{with main_hub_panel {if {exists mb_exit.btn} {mb_exit.btn set_showing 1}}}"
                     : "{with main_hub_panel {if {exists mb_exit.btn} {mb_exit.btn set_showing 0}}}");
}

// DataNode MainHubPanel::Handle(this, DataArray* msg, bool warn): r3 is the
// DataNode it returns, r4 this, r5 msg. Selecting Quit opens the confirm
// screen; the panel's own scripts don't know the button.
extern "C" REX_FUNC(MainHubPanel__Handle_82622F20) {
    const uint32_t ret = ctx.r3.u32;
    const uint32_t msg = ctx.r5.u32;
    if (!msg || !IsQuitSelect(base, msg)) {
        __imp__MainHubPanel__Handle_82622F20(ctx, base);
        return;
    }

    REXLOG_INFO("Main menu: Quit selected; asking to confirm");
    RunInMainDir(ctx, base, "{ui push_screen confirm_exit_game_screen}");
    REX_STORE_U32(ret, 1);
    REX_STORE_U32(ret + 4, band3::kDataInt);
    ctx.r3.u64 = ret;
}
