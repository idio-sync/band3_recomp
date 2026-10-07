#include "mouse_hover.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include "generated/band3_init.h"
#include "src/Input/input_system.h"
#include "src/Input/mouse_menus_driver.h"
#include "src/Render/native_view.h"
#include "src/Render/present_model.h"
#include "src/Render/renderer_switch.h"
#include "src/settings.h"

// Pointing the mouse at a menu's buttons and lists' rows (mouse_hover.h).
// What was drawn where is kept as it's drawn, through the camera selected
// then (RndCam::sCurrent): UIList::DrawShowing hands UIListDir::DrawWidgets
// the list's state and world transform, and DrawWidgets builds where each row
// goes (BuildDrawState); UILabel::DrawShowing draws a label's (or button's)
// text at its world transform, at the size and alignment it keeps. The next
// frame, when the pointer has moved:
// - on the focused list, its row under the pointer is selected with
//   UIList::SetSelectedSimulateScroll, which steps the list there as presses
//   would, so the screen gets the scroll messages its scripts act on (the
//   music library's details);
// - elsewhere, the focused panel's component under it that a controller could
//   move to takes the focus (PanelDir::SetFocusComponent), as a press would.
// Never while a player's overshell menu has the input, and only rows the
// list can highlight without scrolling, so nothing runs on under a still
// pointer.
//
// Addresses and offsets are TU5's, from rb3-xenon (ui/UI.h, ui/UIList.h,
// ui/UIListDir.cpp, ui/UIListWidget.h, ui/UIListState.h, ui/UIComponent.h,
// ui/UILabel.h, ui/PanelDir.h, ui/UIPanel.h, rndobj/Cam.h, rndobj/Text.h,
// meta_band/BandUI.h) and checked against the recompiled code.

extern "C" void __imp__UIListDir__DrawWidgets(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__UIListDir__BuildDrawState(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__UILabel__DrawShowing(PPCContext& ctx, uint8_t* base);
REX_EXTERN(UIManager__FocusComponent);
REX_EXTERN(UIManager__FocusPanel);
REX_EXTERN(PanelDir__SetFocusComponent);
REX_EXTERN(JoypadControllerTypePadNum);
REX_EXTERN(UIListState__ScrollMaxDisplay);
REX_EXTERN(UIListState__MaxFirstShowing);
REX_EXTERN(OvershellPanel__AreAllLocalSlotsAllowingInputToShell);
REX_EXTERN(UIList__CalcBoundingBox);
REX_EXTERN(UIList__SetSelectedSimulateScroll_827F9008);

namespace band3::mouse_hover {

namespace {

using input::mouse_hover::Box2;
using input::mouse_hover::PictureRect;
using input::mouse_hover::Rect;
using input::mouse_hover::Row;
using input::mouse_hover::Transform;
using input::mouse_hover::Vec2;
using input::mouse_hover::Vec3;

constexpr uint32_t kTheBandUI = 0x82DFD2B0;            // BandUI (UIManager) object
constexpr uint32_t kUIManager_CurrentScreen = 0x2C;    // UIScreen*
constexpr uint32_t kBandUI_Overshell = 0xA0;           // OvershellPanel*
constexpr uint32_t kUIScreen_Name = 0x18;              // Symbol (char*)
constexpr uint32_t kRndCam_Current = 0x82CC2928;       // RndCam::sCurrent, set by Select
constexpr uint32_t kRndCam_WorldProjectXfm = 0x174;    // Transform
constexpr uint32_t kRndCam_ScreenRect = 0x2CC;         // Hmx::Rect
constexpr uint32_t kUIList_ListState = 0x188;          // UIListState mListState
// UIListState
constexpr uint32_t kListState_Circular = 0x0;          // bool
constexpr uint32_t kListState_NumDisplay = 0x4;
constexpr uint32_t kListState_MinDisplay = 0x10;
constexpr uint32_t kListState_ScrollPastMin = 0x14;    // bool
constexpr uint32_t kListState_FirstShowing = 0x30;
constexpr uint32_t kListState_SelectedDisplay = 0x38;
// UIListWidgetDrawState
constexpr uint32_t kDrawState_HighlightDisplay = 0x30;  // int
constexpr uint32_t kDrawState_Elements = 0x38;          // vector<UIListElementDrawState>
// UIListElementDrawState
constexpr uint32_t kElement_Size = 0x2C;
constexpr uint32_t kElement_Active = 0x0;   // bool
constexpr uint32_t kElement_Pos = 0x4;      // x, y, z
constexpr uint32_t kElement_Alpha = 0x14;
constexpr uint32_t kElement_State = 0x18;   // UIListWidgetState
constexpr uint32_t kElement_Display = 0x20;
constexpr uint32_t kElement_Showing = 0x24;
constexpr uint32_t kElement_Data = 0x28;
constexpr uint32_t kWidgetInactive = 2;     // kUIListWidgetInactive
// UIComponent (RndDrawable at 0, RndTransformable at 0x24)
constexpr uint32_t kUIComponent_WorldXfm = 0x80;       // RndTransformable::mWorldXfm
constexpr uint32_t kUIComponent_State = 0xE0;          // UIComponent::State
constexpr uint32_t kUIComponent_NavRight = 0xEC;       // ObjPtr<UIComponent> mNavRight's pointer
constexpr uint32_t kUIComponent_NavDown = 0xF8;        // ObjPtr<UIComponent> mNavDown's pointer
constexpr uint32_t kStateDisabled = 2;
constexpr uint32_t kUIComponent_CanHaveFocus = 0x44;   // its vtable slot
// UILabel, and its RndText
constexpr uint32_t kUILabel_Text = 0x144;              // RndText*
constexpr uint32_t kUILabel_Alignment = 0x184;         // RndText::Alignment
constexpr uint32_t kUILabel_Alpha = 0x1BC;             // float; DrawShowing draws nothing at 0
constexpr uint32_t kRndText_CurHeight = 0x184;         // the text's size, as last
constexpr uint32_t kRndText_CurWidth = 0x188;          // GetCurrentStringDimensions
// UIPanel and PanelDir
constexpr uint32_t kUIPanel_Dir = 0x8;                 // PanelDir*
constexpr uint32_t kPanelDir_Components = 0x1F8;       // std::list<UIComponent*>
// more components than any panel has: past it, the list read is wrong
constexpr uint32_t kMaxComponents = 512;
// a row still fading in or out at the list's ends can't be picked: the list
// scrolls on from there, and would scroll under the pointer
constexpr float kOpaque = 0.99f;
// more rows than any list draws: past it, the vector read is wrong
constexpr uint32_t kMaxElements = 256;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

float LoadFloat(uint8_t* base, uint32_t addr) { return std::bit_cast<float>(Load32(base, addr)); }

Vec3 LoadVec3(uint8_t* base, uint32_t addr) {
    return {LoadFloat(base, addr), LoadFloat(base, addr + 4), LoadFloat(base, addr + 8)};
}

// a Transform: three Vector3 rows and the translation, each 16 bytes
Transform LoadTransform(uint8_t* base, uint32_t addr) {
    Transform t;
    for (uint32_t i = 0; i < 3; i++) t.rows[i] = LoadVec3(base, addr + i * 16);
    t.v = LoadVec3(base, addr + 48);
    return t;
}

// a stack below the hooked function's frame, with the caller's r13, and
// room above it for what a call writes back
constexpr uint32_t kCallStack = 0x200;
constexpr uint32_t kScratch = 0x80;

PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - kCallStack;
    call.r13 = ctx.r13;
    return call;
}

struct Element {
    Vec3 pos;
    float alpha = 1;
    uint32_t state = 0;
    int display = 0, showing = 0, data = 0;
};

// one list as it was drawn
struct Drawn {
    uint32_t list_state = 0;
    Transform world;
    Transform world_project;
    Rect screen_rect;
    bool camera = false;
    int highlight_display = -1;
    std::vector<Element> elements;
};

// Game thread. The lists being drawn now (a list's rows can draw a sub
// list), those drawn this frame, and those the last.
std::vector<Drawn> g_drawing;
std::vector<Drawn> g_frame;
std::vector<Drawn> g_last;

// a label (a button's too) as it was drawn: its text's corners on the
// picture, 0..1
struct DrawnLabel {
    uint32_t label = 0;
    std::array<Vec2, 4> corners;
};
std::vector<DrawnLabel> g_labels_frame;
std::vector<DrawnLabel> g_labels_last;
bool g_record = false;
// the pointer move last acted on
uint64_t g_seen = 0;
// how long after a move the draws are still kept, and until when they are
constexpr auto kRecordFor = std::chrono::seconds(10);
std::chrono::steady_clock::time_point g_record_until{};

// the harness's report, asked for from its thread
std::mutex g_report_mutex;
std::condition_variable g_report_cv;
bool g_report_wanted = false;
uint32_t g_report_width = 0, g_report_height = 0;
std::optional<Report> g_report;

void ReadElements(uint8_t* base, uint32_t draw_state, Drawn& drawn) {
    drawn.highlight_display = static_cast<int32_t>(Load32(base, draw_state + kDrawState_HighlightDisplay));
    const uint32_t begin = Load32(base, draw_state + kDrawState_Elements);
    const uint32_t end = Load32(base, draw_state + kDrawState_Elements + 4);
    if (!begin || end < begin || (end - begin) % kElement_Size) return;
    const uint32_t count = (end - begin) / kElement_Size;
    if (count > kMaxElements) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t e = begin + i * kElement_Size;
        if (!*rex::memory::GuestPtr<uint8_t*>(base, e + kElement_Active)) continue;
        drawn.elements.push_back(Element{
            .pos = LoadVec3(base, e + kElement_Pos),
            .alpha = LoadFloat(base, e + kElement_Alpha),
            .state = Load32(base, e + kElement_State),
            .display = static_cast<int32_t>(Load32(base, e + kElement_Display)),
            .showing = static_cast<int32_t>(Load32(base, e + kElement_Showing)),
            .data = static_cast<int32_t>(Load32(base, e + kElement_Data)),
        });
    }
}

// an object's address and class, from the compiler's RTTI: the vtable's
// complete object locator, before it, has the type descriptor, whose name
// follows its vtable pointer and a spare word (".?AVUIList@@")
std::string Describe(uint8_t* base, uint32_t object) {
    if (!object) return "none";
    std::string out = std::format("{:08X}", object);
    const uint32_t vtable = Load32(base, object);
    const uint32_t locator = vtable ? Load32(base, vtable - 4) : 0;
    const uint32_t type = locator ? Load32(base, locator + 12) : 0;
    if (!type) return out;
    const char* name = rex::memory::GuestPtr<const char*>(base, type + 8);
    if (std::strncmp(name, ".?AV", 4) != 0) return out;
    return out + " " + std::string(name + 4, std::strcspn(name + 4, "@"));
}

std::string ScreenName(uint8_t* base) {
    const uint32_t screen = Load32(base, kTheBandUI + kUIManager_CurrentScreen);
    const uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
    return name ? rex::memory::GuestPtr<const char*>(base, name) : "";
}

// where the picture lies in a window of this size, as the window shows it:
// the native renderer fills it (native_fill_window) or letterboxes it, as
// the SDK's presenter does the emulated GPU's (present_letterbox)
PictureRect PictureFor(uint32_t width, uint32_t height) {
    const bool sdk_letterbox = !rex::cvar::GetFlagInfo("present_letterbox") ||
                               rex::cvar::Query<bool>("present_letterbox");
    const bool letterbox = render::ShowsNativePicture()
                               ? !REXCVAR_GET(native_fill_window) && sdk_letterbox
                               : sdk_letterbox;
    const render::ImageRect r = render::LetterboxRect(width, height, letterbox);
    return {int32_t(r.x), int32_t(r.y), int32_t(r.w), int32_t(r.h)};
}

// a list as the hover sees it, in the window's pixels
struct View {
    uint32_t list = 0;
    std::vector<Row> rows;
    std::vector<ReportRow> report_rows;
    Box2 bounds;
    int highlighted = -1;
};

std::optional<Vec2> OnWindow(const Transform& world_project, const Rect& screen_rect,
                             const PictureRect& picture, const Vec3& world) {
    const auto at = input::mouse_hover::Project(world_project, screen_rect, world);
    if (!at) return std::nullopt;
    return Vec2{picture.x + at->x * picture.w, picture.y + at->y * picture.h};
}

uint32_t FocusComponent(PPCContext& ctx, uint8_t* base) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = kTheBandUI;
    UIManager__FocusComponent(call, base);
    return call.r3.u32;
}

int32_t CallInt(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*), uint32_t arg) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = arg;
    function(call, base);
    return call.r3.s32;
}

input::mouse_hover::ListState ReadListState(PPCContext& ctx, uint8_t* base, uint32_t state) {
    const auto byte = [&](uint32_t offset) { return *rex::memory::GuestPtr<uint8_t*>(base, state + offset) != 0; };
    const auto word = [&](uint32_t offset) { return static_cast<int32_t>(Load32(base, state + offset)); };
    return {.circular = byte(kListState_Circular),
            .num_display = word(kListState_NumDisplay),
            .min_display = word(kListState_MinDisplay),
            .scroll_past_min = byte(kListState_ScrollPastMin),
            .scroll_max_display = CallInt(ctx, base, UIListState__ScrollMaxDisplay, state),
            .first_showing = word(kListState_FirstShowing),
            .max_first_showing = CallInt(ctx, base, UIListState__MaxFirstShowing, state),
            .selected_display = word(kListState_SelectedDisplay)};
}

// `component`, if it's a list drawn last frame
std::optional<View> ListView(PPCContext& ctx, uint8_t* base, const PictureRect& picture,
                             uint32_t component) {
    const Drawn* drawn = nullptr;
    for (const Drawn& d : g_last) {
        if (component && d.list_state == component + kUIList_ListState) drawn = &d;
    }
    if (!drawn) return std::nullopt;

    View view;
    view.list = component;
    const input::mouse_hover::ListState band = ReadListState(ctx, base, drawn->list_state);
    for (const Element& e : drawn->elements) {
        // UIListWidget::CalcXfm: the row's x and z from the list's origin
        const auto at = OnWindow(drawn->world_project, drawn->screen_rect, picture,
                                 Apply(drawn->world, Vec3{e.pos.x, 0, e.pos.z}));
        if (!at) continue;
        const bool pickable =
            e.state != kWidgetInactive && e.alpha >= kOpaque && StaysPut(band, e.display);
        const bool highlighted = e.display == drawn->highlight_display;
        if (highlighted) view.highlighted = e.showing;
        view.rows.push_back(Row{.showing = e.showing, .at = *at, .pickable = pickable});
        view.report_rows.push_back(ReportRow{.display = e.display,
                                             .showing = e.showing,
                                             .data = e.data,
                                             .at = *at,
                                             .pickable = pickable,
                                             .highlighted = highlighted});
    }

    // UIList::CalcBoundingBox(Box&): the world box of everything the list
    // draws, its rows and their widgets
    const uint32_t box = ctx.r1.u32 - kScratch;
    PPCContext call = CallContext(ctx);
    call.r3.u64 = component;
    call.r4.u64 = box;
    UIList__CalcBoundingBox(call, base);
    const Vec3 lo = LoadVec3(base, box), hi = LoadVec3(base, box + 16);
    std::vector<Vec2> corners;
    for (int corner = 0; corner < 8; corner++) {
        const Vec3 p{corner & 1 ? hi.x : lo.x, corner & 2 ? hi.y : lo.y, corner & 4 ? hi.z : lo.z};
        if (const auto at = OnWindow(drawn->world_project, drawn->screen_rect, picture, p)) {
            corners.push_back(*at);
        }
    }
    view.bounds = input::mouse_hover::Around(corners);
    return view;
}

// UIComponent::CanHaveFocus(), a virtual
bool CanHaveFocus(PPCContext& outer, uint8_t* base, uint32_t component) {
    const uint32_t vtable = Load32(base, component);
    if (!vtable) return false;
    PPCContext ctx = CallContext(outer);
    ctx.r3.u64 = component;
    REX_CALL_INDIRECT_FUNC(Load32(base, vtable + kUIComponent_CanHaveFocus));
    return ctx.r3.u32 & 0xFF;
}

// The focused panel's components the pointer could move the focus to: those
// a controller could move it to (joined to the focus by the components' down
// and right links, either way, as PanelDir::ComponentNav follows them up,
// down, left and right, through disabled ones), that can have it and aren't
// disabled, drawn last frame as a list or a label (buttons are labels), with
// their boxes in the window's pixels. Also the panel (PanelDir), for
// SetFocusComponent.
std::vector<input::mouse_hover::Target> FocusTargets(PPCContext& ctx, uint8_t* base,
                                                     const PictureRect& picture, uint32_t focus,
                                                     uint32_t& panel_dir) {
    std::vector<input::mouse_hover::Target> targets;
    PPCContext call = CallContext(ctx);
    call.r3.u64 = kTheBandUI;
    UIManager__FocusPanel(call, base);
    const uint32_t panel = call.r3.u32;
    panel_dir = panel ? Load32(base, panel + kUIPanel_Dir) : 0;
    if (!panel_dir) return targets;

    // STLport's std::list<UIComponent*> holds its sentinel node itself: the
    // list's first word is the first node, and the list's address is the end
    std::vector<uint32_t> components;
    const uint32_t head = panel_dir + kPanelDir_Components;
    uint32_t node = Load32(base, head);
    for (uint32_t i = 0; node && node != head && i < kMaxComponents; i++, node = Load32(base, node)) {
        if (const uint32_t component = Load32(base, node + 8)) components.push_back(component);
    }
    std::vector<std::pair<uint32_t, uint32_t>> links;
    for (const uint32_t component : components) {
        for (const uint32_t link : {kUIComponent_NavDown, kUIComponent_NavRight}) {
            if (const uint32_t to = Load32(base, component + link)) links.emplace_back(component, to);
        }
    }
    const std::vector<uint32_t> reachable = input::mouse_hover::Joined(links, focus);

    for (const uint32_t component : reachable) {
        if (Load32(base, component + kUIComponent_State) == kStateDisabled) continue;
        Box2 box;
        bool found = false;
        for (const DrawnLabel& label : g_labels_last) {
            if (label.label != component) continue;
            std::vector<Vec2> corners;
            for (const Vec2& c : label.corners) {
                corners.push_back({picture.x + c.x * picture.w, picture.y + c.y * picture.h});
            }
            box = input::mouse_hover::Around(corners);
            found = true;
        }
        if (!found) {
            if (const auto view = ListView(ctx, base, picture, component)) {
                box = view->bounds;
                found = true;
            }
        }
        if (!found || !CanHaveFocus(ctx, base, component)) continue;
        targets.push_back({component, box});
    }
    return targets;
}

// A player's overshell menu (Start on a menu) takes their controller's
// presses, the mouse's with them, and leaves the screen's focus where it was:
// the hover leaves it there too. A slot "allows input to the shell" while its
// menu is closed, passing its presses on to the screen.
bool OvershellTakesInput(PPCContext& ctx, uint8_t* base) {
    const uint32_t overshell = Load32(base, kTheBandUI + kBandUI_Overshell);
    return overshell &&
           !(CallInt(ctx, base, OvershellPanel__AreAllLocalSlotsAllowingInputToShell, overshell) & 0xFF);
}

bool HoverAllowed(PPCContext& ctx, uint8_t* base) {
    return REXCVAR_GET(mouse_menus) && !render::InSong() && !input::GameInputBlocked() &&
           !OvershellTakesInput(ctx, base);
}

// selects the list's row under the pointer; whether the pointer is on the list
bool HoverRow(PPCContext& ctx, uint8_t* base, const View& view, const Vec2& pointer) {
    if (!view.bounds.Contains(pointer)) return false;
    const auto row = input::mouse_hover::RowAt(view.rows, view.bounds, pointer);
    if (!row || *row == view.highlighted) return true;
    REXLOG_DEBUG("mouse: hovering row {} of the list on {}", *row, ScreenName(base));
    PPCContext call = CallContext(ctx);
    call.r3.u64 = view.list;
    call.r4.s64 = *row;
    UIList__SetSelectedSimulateScroll_827F9008(call, base);
    return true;
}

// The focus change's nav type, as PanelDir::PanelNav gives it: the
// controller type of the pad that moved it (JoypadControllerTypePadNum), here
// the player's the mouse presses for. Scripts pick the move's sound by it.
uint32_t NavType(PPCContext& ctx, uint8_t* base) {
    const uint32_t symbol = ctx.r1.u32 - kScratch;
    PPCContext call = CallContext(ctx);
    call.r3.u64 = symbol;
    call.r4.u64 = input::MouseMenuPlayer();
    JoypadControllerTypePadNum(call, base);
    return Load32(base, symbol);
}

void Hover(PPCContext& ctx, uint8_t* base, const input::HoverPointer& pointer) {
    if (!HoverAllowed(ctx, base)) return;
    const PictureRect picture = PictureFor(pointer.width, pointer.height);
    const Vec2 at{float(pointer.x), float(pointer.y)};
    const uint32_t focus = FocusComponent(ctx, base);
    if (const auto view = ListView(ctx, base, picture, focus)) {
        if (HoverRow(ctx, base, *view, at)) return;
    }
    uint32_t panel_dir = 0;
    const auto targets = FocusTargets(ctx, base, picture, focus, panel_dir);
    const uint32_t target = input::mouse_hover::TargetAt(targets, at);
    if (!target || target == focus) return;
    REXLOG_DEBUG("mouse: hovering {} on {}", Describe(base, target), ScreenName(base));
    // PanelDir::SetFocusComponent(PanelDir*, UIComponent*, Symbol nav_type),
    // which tells the screen as a press would (UIComponentFocusChangeMsg)
    const uint32_t nav_type = NavType(ctx, base);
    PPCContext call = CallContext(ctx);
    call.r3.u64 = panel_dir;
    call.r4.u64 = target;
    call.r5.u64 = nav_type;
    PanelDir__SetFocusComponent(call, base);
    // a list taking the focus takes the row under the pointer too
    if (const auto view = ListView(ctx, base, picture, target)) HoverRow(ctx, base, *view, at);
}

bool ReportWanted() {
    std::lock_guard<std::mutex> lock(g_report_mutex);
    return g_report_wanted;
}

void AnswerReport(PPCContext& ctx, uint8_t* base) {
    uint32_t width, height;
    {
        std::lock_guard<std::mutex> lock(g_report_mutex);
        if (!g_report_wanted) return;
        width = g_report_width;
        height = g_report_height;
    }
    Report report;
    report.screen = ScreenName(base);
    report.picture = PictureFor(width, height);
    report.moves = g_seen;
    report.focus = Describe(base, FocusComponent(ctx, base));
    for (const Drawn& d : g_last) report.drawn.push_back(Describe(base, d.list_state - kUIList_ListState));
    uint32_t panel_dir = 0;
    for (const auto& target : FocusTargets(ctx, base, report.picture, FocusComponent(ctx, base), panel_dir)) {
        report.targets.push_back({Describe(base, target.component), target.box});
    }
    if (auto view = ListView(ctx, base, report.picture, FocusComponent(ctx, base))) {
        report.list = true;
        report.bounds = view->bounds;
        report.rows = std::move(view->report_rows);
    }
    {
        std::lock_guard<std::mutex> lock(g_report_mutex);
        g_report = std::move(report);
        g_report_wanted = false;
    }
    g_report_cv.notify_all();
}

}

void RunFrame(PPCContext& ctx, uint8_t* base) {
    const bool recorded = g_record;
    g_last.swap(g_frame);
    g_frame.clear();
    g_labels_last.swap(g_labels_frame);
    g_labels_frame.clear();

    // The draws are kept only while the mouse is in use, so a controller
    // costs nothing: a move (or the harness asking) starts it, and it goes
    // on for kRecordFor after. A move that starts it waits a frame for the
    // draws to go by.
    const auto now = std::chrono::steady_clock::now();
    const auto pointer = input::TakeHoverPointer(g_seen);
    if (pointer || ReportWanted()) g_record_until = now + kRecordFor;
    const bool on = REXCVAR_GET(mouse_menus);
    g_record = on && now < g_record_until;
    if (on && !recorded) return;

    AnswerReport(ctx, base);
    if (!pointer) return;
    g_seen = pointer->moves;
    Hover(ctx, base, *pointer);
    input::HoverApplied(pointer->moves);
}

std::optional<Report> RequestReport(uint32_t width, uint32_t height, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(g_report_mutex);
    g_report.reset();
    g_report_wanted = true;
    g_report_width = width;
    g_report_height = height;
    if (!g_report_cv.wait_for(lock, wait, [] { return g_report.has_value(); })) {
        g_report_wanted = false;
        return std::nullopt;
    }
    return std::move(g_report);
}

}

// UIListDir::DrawWidgets(UIListDir*, const UIListState&, vector<UIListWidget*>&,
// const Transform& world, UIComponent::State, Box* bounds, bool): a list's
// draw, or with `bounds` its bounding box (UIList::CalcBoundingBox), which
// isn't kept
extern "C" REX_FUNC(UIListDir__DrawWidgets)
{
    using namespace band3::mouse_hover;
    if (!g_record || ctx.r8.u32) {
        __imp__UIListDir__DrawWidgets(ctx, base);
        return;
    }
    Drawn drawn;
    drawn.list_state = ctx.r4.u32;
    drawn.world = LoadTransform(base, ctx.r6.u32);
    if (const uint32_t cam = Load32(base, kRndCam_Current)) {
        drawn.world_project = LoadTransform(base, cam + kRndCam_WorldProjectXfm);
        drawn.screen_rect = {LoadFloat(base, cam + kRndCam_ScreenRect),
                             LoadFloat(base, cam + kRndCam_ScreenRect + 4),
                             LoadFloat(base, cam + kRndCam_ScreenRect + 8),
                             LoadFloat(base, cam + kRndCam_ScreenRect + 12)};
        drawn.camera = true;
    }
    g_drawing.push_back(std::move(drawn));
    __imp__UIListDir__DrawWidgets(ctx, base);
    Drawn done = std::move(g_drawing.back());
    g_drawing.pop_back();
    if (done.camera && !done.elements.empty()) g_frame.push_back(std::move(done));
}

// UILabel::DrawShowing(UILabel*): a label's (or button's) draw, through the
// camera selected then, at its world transform
extern "C" REX_FUNC(UILabel__DrawShowing)
{
    using namespace band3::mouse_hover;
    const uint32_t label = ctx.r3.u32;
    __imp__UILabel__DrawShowing(ctx, base);
    if (!g_record || !(LoadFloat(base, label + kUILabel_Alpha) > 0)) return;
    const uint32_t cam = Load32(base, kRndCam_Current);
    const uint32_t text = Load32(base, label + kUILabel_Text);
    if (!cam || !text) return;
    const Transform world_project = LoadTransform(base, cam + kRndCam_WorldProjectXfm);
    const Rect screen_rect{LoadFloat(base, cam + kRndCam_ScreenRect),
                           LoadFloat(base, cam + kRndCam_ScreenRect + 4),
                           LoadFloat(base, cam + kRndCam_ScreenRect + 8),
                           LoadFloat(base, cam + kRndCam_ScreenRect + 12)};
    const Transform world = LoadTransform(base, label + kUIComponent_WorldXfm);
    const auto corners = band3::input::mouse_hover::LabelCorners(
        Load32(base, label + kUILabel_Alignment), LoadFloat(base, text + kRndText_CurWidth),
        LoadFloat(base, text + kRndText_CurHeight));
    DrawnLabel drawn{.label = label};
    for (size_t i = 0; i < corners.size(); i++) {
        const auto at =
            band3::input::mouse_hover::Project(world_project, screen_rect, Apply(world, corners[i]));
        if (!at) return;
        drawn.corners[i] = *at;
    }
    g_labels_frame.push_back(drawn);
}

// UIListDir::BuildDrawState(UIListDir*, UIListWidgetDrawState&, const
// UIListState&, UIComponent::State, float): where DrawWidgets puts each row
extern "C" REX_FUNC(UIListDir__BuildDrawState)
{
    using namespace band3::mouse_hover;
    const uint32_t draw_state = ctx.r4.u32;
    const uint32_t list_state = ctx.r5.u32;
    __imp__UIListDir__BuildDrawState(ctx, base);
    if (g_drawing.empty()) return;
    Drawn& drawn = g_drawing.back();
    if (drawn.list_state != list_state || !drawn.elements.empty()) return;
    ReadElements(base, draw_state, drawn);
}
