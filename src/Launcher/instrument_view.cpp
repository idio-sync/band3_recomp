#include "instrument_view.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include "src/Input/midi_drums_driver.h"
#include "src/settings.h"
#include "launcher_style.h"

namespace band3::launcher {

using input::DeviceKind;
namespace xbox = input::xbox;

namespace {

// Rock Band's colours
constexpr ImVec4 kGreen(0.27f, 0.78f, 0.33f, 1.0f);
constexpr ImVec4 kRed(0.92f, 0.26f, 0.26f, 1.0f);
constexpr ImVec4 kYellow(0.98f, 0.82f, 0.18f, 1.0f);
constexpr ImVec4 kBlue(0.24f, 0.52f, 0.96f, 1.0f);
constexpr ImVec4 kOrange(0.98f, 0.56f, 0.16f, 1.0f);

constexpr ImVec4 kFretColors[input::kFretCount] = {kGreen, kRed, kYellow, kBlue, kOrange};
constexpr ImVec4 kPadColors[input::kPadCount] = {kRed, kYellow, kBlue, kGreen};
constexpr ImVec4 kCymbalColors[input::kCymbalCount] = {kYellow, kBlue, kGreen};

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// a part in its colour, lit from 0 (a dim version on the frame colour) to 1
ImU32 Lit(const ImVec4& color, float level) {
    const ImVec4 dim = Mix(kFrame, color, 0.22f);
    return ImGui::GetColorU32(Mix(dim, color, std::clamp(level, 0.0f, 1.0f)));
}

// text on a part: dark on a brightly lit one, light otherwise
ImU32 TextOn(float level) { return ImGui::GetColorU32(level > 0.55f ? kBackground : kText); }

void CenteredText(ImDrawList* draw, const ImVec2& center, ImU32 color, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(center.x - size.x / 2, center.y - size.y / 2), color, text);
}

// a rounded button-like label, lit in the accent colour; returns its width
float Pill(ImDrawList* draw, const ImVec2& at, const char* label, bool lit) {
    const ImVec2 text = ImGui::CalcTextSize(label);
    const ImVec2 size(text.x + Px(18), Px(24));
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y),
                        ImGui::GetColorU32(lit ? kAccent : kFrame), size.y / 2);
    CenteredText(draw, ImVec2(at.x + size.x / 2, at.y + size.y / 2),
                 ImGui::GetColorU32(lit ? ImVec4(1, 1, 1, 1) : kMuted), label);
    return size.x;
}

// a bar filled from the left, 0 to 1
void Bar(ImDrawList* draw, const ImVec2& at, const ImVec2& size, float fill, const ImVec4& color) {
    const ImVec2 end(at.x + size.x, at.y + size.y);
    draw->AddRectFilled(at, end, ImGui::GetColorU32(kFrame), size.y / 2);
    fill = std::clamp(fill, 0.0f, 1.0f);
    if (fill > 0) {
        draw->AddRectFilled(at, ImVec2(at.x + std::max(size.x * fill, size.y), end.y),
                            ImGui::GetColorU32(color), size.y / 2);
    }
}

// a muted label in the small font, its middle at `y`
void Label(ImDrawList* draw, float x, float y, const char* text) {
    draw->AddText(ImVec2(x, y - ImGui::GetFontSize() / 2), ImGui::GetColorU32(kMuted), text);
}

// a stick: where it points, ringed in the accent colour while pressed in
void Stick(ImDrawList* draw, const ImVec2& center, float radius, int16_t x, int16_t y,
           bool pressed) {
    draw->AddCircleFilled(center, radius, ImGui::GetColorU32(kFrame));
    if (pressed) {
        draw->AddCircle(center, radius, ImGui::GetColorU32(kAccent), 0, std::max(2.0f, Px(3)));
    }
    const float travel = radius - Px(9);
    const ImVec2 dot(center.x + travel * static_cast<float>(x) / 32768.0f,
                     center.y - travel * static_cast<float>(y) / 32768.0f);
    draw->AddCircleFilled(dot, Px(8), ImGui::GetColorU32(kText));
}

}

void DevicePanel::Poll(bool showing) {
    const Clock::time_point now = Clock::now();
    if (!listed_ || now >= next_list_) {
        // the capabilities are read again for the new list
        devices_.clear();
        for (input::InputDevice& info : input::PlayerDevices()) {
            Device& device = devices_.emplace_back();
            device.info = std::move(info);
        }
        listed_ = true;
        next_list_ = now + std::chrono::milliseconds(250);
        ChooseDevice();
    }
    std::vector<uint64_t> connected;
    connected.reserve(devices_.size());
    for (const Device& device : devices_) connected.push_back(device.info.id);
    test_.Follow(connected, showing);

    nav_pads_.clear();
    for (Device& device : devices_) {
        const bool nav = DrivesNavigation(device.info.kind);
        // the selected one is drawn only while the tab shows
        const bool shown = showing && selected_ == device.info.id;
        device.state.reset();
        if (!nav && !shown) continue;
        if (!device.caps_read) {
            device.caps = input::ReadInputCaps(device.info.id);
            device.caps_read = true;
            if (device.caps) sub_types_[device.info.id] = device.caps->sub_type;
        }
        if (!device.caps) continue;
        device.state = input::ReadInputState(device.info.id);
        if (nav && device.state) {
            nav_pads_.push_back(
                test_.Filter(device.info.id, NavFromReading(*device.caps, *device.state)));
        }
    }
}

void DevicePanel::ChooseDevice() {
    auto connected = [&](uint64_t id) {
        return std::ranges::any_of(devices_, [&](const Device& d) { return d.info.id == id; });
    };
    if (picked_ && selected_ && connected(*selected_)) return;
    picked_ = false;
    selected_.reset();
    // the first that plays and isn't the keyboard, else the first there is
    for (const Device& d : devices_) {
        if (DrivesNavigation(d.info.kind) && d.info.player > 0) {
            selected_ = d.info.id;
            return;
        }
    }
    for (const Device& d : devices_) {
        if (d.info.kind != DeviceKind::kSdlCopy) {
            selected_ = d.info.id;
            return;
        }
    }
}

void DevicePanel::Draw() {
    SectionHeading("Devices");
    DrawList();
    for (const Device& device : devices_) {
        if (selected_ == device.info.id) DrawTestView(device);
    }
    ImGui::Dummy(ImVec2(0, Px(14)));
}

void DevicePanel::DrawList() {
    if (listed_ && devices_.empty()) {
        ImGui::TextColored(kMuted, "No controllers found. Connect one, and it shows up here.");
        return;
    }
    const float row = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
    if (ImGui::BeginTable("##devices", 3, ImGuiTableFlags_None)) {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::TableSetupColumn("device", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed,
                                std::clamp(width * 0.34f, Px(200), Px(400)));
        ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthFixed, Px(130));
        for (const Device& device : devices_) {
            const input::InputDevice& d = device.info;
            ImGui::PushID(static_cast<int>(d.id ^ (d.id >> 32)));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
            ImGui::TableSetColumnIndex(0);
            const auto sub_type = sub_types_.find(d.id);
            const std::string kind = DeviceKindLabel(
                d.kind, d.name,
                sub_type == sub_types_.end() ? std::nullopt
                                             : std::optional<uint8_t>(sub_type->second));
            const std::string name = d.name.empty() ? "(unnamed)" : d.name;
            // SDL's copy of a dongle instrument is listed so it doesn't look
            // missing, but there's nothing to test: its dongle's row is it
            const bool copy = d.kind == DeviceKind::kSdlCopy;
            ImGui::BeginDisabled(copy);
            if (ImGui::Selectable(name.c_str(), selected_ == d.id,
                                  ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, row))) {
                selected_ = d.id;
                picked_ = true;
                // the keyboard navigates by itself, so it's only shown
                if (DrivesNavigation(d.kind)) {
                    test_.Begin(d.id);
                } else {
                    test_.End();
                }
            }
            ImGui::EndDisabled();
            if (copy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("SDL's copy of an instrument band3 reads through its USB dongle.\n"
                                  "The game doesn't play this copy.");
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kMuted, "%s", kind.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(d.player > 0 ? kText : kMuted, "%s", PlayerLabel(d.player).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void DevicePanel::DrawTestView(const Device& device) {
    const input::InputDevice& d = device.info;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPopup);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(18), Px(14)));
    // (flattened: its Test button is on the page's navigation)
    if (ImGui::BeginChild("##test_view", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar)) {
        // the keyboard navigates by itself, so it's only shown, not tested
        const bool testable = DrivesNavigation(d.kind);
        const bool testing = test_.Testing() == d.id;
        if (testable) ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(d.name.empty() ? "(unnamed)" : d.name.c_str());
        if (testable) {
            // Test and Stop are one button, so the focus stays on it
            const float width =
                std::max(ImGui::CalcTextSize("Test").x, ImGui::CalcTextSize("Stop").x) +
                ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
            if (ImGui::Button(testing ? "Stop###test" : "Test###test", ImVec2(width, 0))) {
                if (testing) {
                    test_.End();
                } else {
                    test_.Begin(d.id);
                }
            }
        }
        FontScope font(kSmallSize);
        ImGui::PushTextWrapPos(0);
        if (testing) {
            ImGui::TextColored(kAccent, "Testing: press Back on it (or click Stop) to stop. Until "
                                        "then it plays this view only, not the launcher.");
        } else if (testable) {
            ImGui::TextColored(kMuted, "It also moves around the launcher. Click its row, or press "
                                       "A on it, to test it on its own.");
        }
        if (!device.caps || !device.state) {
            ImGui::TextColored(kMuted, "Can't read it right now.");
        } else {
            const input::Caps360& caps = *device.caps;
            const input::Gamepad360& state = *device.state;
            const TestView view = ViewFor(caps);
            const bool gamepad = caps.sub_type == input::kSubtypeGamepad;
            if (IsStandIn(d.kind, d.name)) {
                ImGui::TextColored(kMuted, "The SDK's stand-in for a controller: it never presses "
                                           "anything.");
            } else if (gamepad &&
                       (d.kind == DeviceKind::kPad || d.kind == DeviceKind::kSynthetic)) {
                // the game plays it as controller_type (settings.cpp)
                const auto plays_as = PlaysAsLabel(REXCVAR_GET(controller_type));
                const char* what = d.kind == DeviceKind::kSynthetic ? "The keyboard" : "It";
                if (plays_as) {
                    ImGui::TextColored(kMuted, "%s plays as %s (Gamepads play as).", what,
                                       plays_as->c_str());
                } else {
                    ImGui::TextColored(kMuted, "%s doesn't play: Gamepads play as is \"Don't "
                                               "override\".", what);
                }
                if (d.kind == DeviceKind::kPad) {
                    ImGui::TextColored(kMuted, "With the SDL input backend an Xbox 360 guitar or "
                                               "drum kit shows here as a controller too, and "
                                               "plays as that.");
                }
            } else if (view == TestView::kPad && !gamepad) {
                ImGui::TextColored(kMuted, "Shown as the buttons and sticks it reports.");
            }
            ImGui::Dummy(ImVec2(0, Px(4)));
            if (hits_for_ != d.id) {
                // the flashes are another device's: start again
                hits_for_ = d.id;
                pad_hits_ = {};
                cymbal_hits_ = {};
                kick_hits_ = {};
            }
            switch (view) {
            case TestView::kGuitar: DrawGuitar(input::DecodeGuitar(state)); break;
            case TestView::kDrums:
                DrawDrums(input::DecodeDrums(state, caps), input::IsRb2Drums(caps));
                break;
            case TestView::kPad: DrawPad(state); break;
            }
            if (d.kind == DeviceKind::kMidiDrums) DrawMidiHits();
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

float DevicePanel::Flash(Hit& hit, uint8_t velocity_now) {
    const Clock::time_point now = Clock::now();
    if (velocity_now > 0) hit = {velocity_now, now};
    return FlashLevel(hit.velocity, std::chrono::duration<float>(now - hit.at).count());
}

void DevicePanel::DrawGuitar(const input::GuitarInputs& in) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float fret = Px(52), gap = Px(10), solo_height = Px(26);
    for (int f = 0; f < input::kFretCount; f++) {
        const float x = o.x + f * (fret + gap);
        const bool held = in.frets[f];
        draw->AddRectFilled(ImVec2(x, o.y), ImVec2(x + fret, o.y + fret),
                            Lit(kFretColors[f], held && !in.solo ? 1.0f : 0.0f), Px(8));
        const float y = o.y + fret + Px(8);
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + fret, y + solo_height),
                            Lit(kFretColors[f], held && in.solo ? 1.0f : 0.0f), Px(6));
    }
    const float frets_end = o.x + static_cast<float>(input::kFretCount) * (fret + gap) - gap;
    Label(draw, frets_end + Px(12), o.y + fret + Px(8) + solo_height / 2, "Solo");

    // strum up and down
    const float cx = frets_end + Px(34);
    const float half = Px(15);
    const ImU32 up = ImGui::GetColorU32(in.strum_up ? kAccent : kFrame);
    const ImU32 down = ImGui::GetColorU32(in.strum_down ? kAccent : kFrame);
    draw->AddTriangleFilled(ImVec2(cx, o.y + Px(2)), ImVec2(cx + half, o.y + Px(23)),
                            ImVec2(cx - half, o.y + Px(23)), up);
    draw->AddTriangleFilled(ImVec2(cx - half, o.y + Px(29)), ImVec2(cx + half, o.y + Px(29)),
                            ImVec2(cx, o.y + Px(50)), down);

    // whammy, tilt, pickup, and the menu buttons
    const float x = cx + half + Px(34);
    const float label = Px(76);
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float bar = std::clamp(right - x - label, Px(80), Px(260));
    const float line = Px(24);
    float y = o.y + line / 2;
    Label(draw, x, y, "Whammy");
    Bar(draw, ImVec2(x + label, y - Px(7)), ImVec2(bar, Px(14)), in.whammy, kAccent);
    y += line;
    Label(draw, x, y, "Tilt");
    Bar(draw, ImVec2(x + label, y - Px(7)), ImVec2(bar, Px(14)), in.tilt, kAccent);
    y += line;
    Label(draw, x, y, "Pickup");
    // RB guitars' five-way switch, spread over the trigger's range
    const int position = (in.pickup * 5) / 256;
    for (int p = 0; p < 5; p++) {
        const ImVec2 c(x + label + Px(7) + p * Px(24), y);
        draw->AddCircleFilled(c, Px(7), ImGui::GetColorU32(p == position ? kAccent : kFrame));
    }
    y += line;
    float px = x + label;
    px += Pill(draw, ImVec2(px, y - Px(12)), "Start", in.nav.start) + Px(8);
    Pill(draw, ImVec2(px, y - Px(12)), "Back", in.nav.back);

    ImGui::Dummy(ImVec2(right - o.x, std::max(fret + Px(8) + solo_height, line * 4)));
}

void DevicePanel::DrawDrums(const input::DrumInputs& in, bool velocity) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float r = Px(30), rc = Px(24), gap = Px(14);
    const float pads_y = o.y + rc * 2 + Px(10) + r;
    auto pad_x = [&](int p) { return o.x + r + p * (r * 2 + gap); };
    char number[8];

    // the cymbals, over the yellow, blue and green pads
    for (int c = 0; c < input::kCymbalCount; c++) {
        const float level = Flash(cymbal_hits_[c], in.cymbals[c]);
        const ImVec2 center(pad_x(input::kYellowPad + c), o.y + rc);
        draw->AddEllipseFilled(center, ImVec2(rc * 1.25f, rc * 0.8f), Lit(kCymbalColors[c], level));
        if (cymbal_hits_[c].velocity > 0 && velocity) {
            std::snprintf(number, sizeof(number), "%u", cymbal_hits_[c].velocity);
            CenteredText(draw, center, TextOn(level), number);
        }
    }
    for (int p = 0; p < input::kPadCount; p++) {
        const float level = Flash(pad_hits_[p], in.pads[p]);
        const ImVec2 center(pad_x(p), pads_y);
        draw->AddCircleFilled(center, r, Lit(kPadColors[p], level));
        if (pad_hits_[p].velocity > 0 && velocity) {
            std::snprintf(number, sizeof(number), "%u", pad_hits_[p].velocity);
            CenteredText(draw, center, TextOn(level), number);
        }
    }
    // both kick pedals, under the pads
    const float pads_end = pad_x(input::kPadCount - 1) + r;
    const float kick_y = pads_y + r + Px(10);
    const float kick_w = (pads_end - o.x - gap) / 2;
    const bool kicks[2] = {in.kick1, in.kick2};
    const char* kick_names[2] = {"Kick", "Kick 2"};
    for (int k = 0; k < 2; k++) {
        const float level = Flash(kick_hits_[k], kicks[k] ? 127 : 0);
        const float x = o.x + k * (kick_w + gap);
        draw->AddRectFilled(ImVec2(x, kick_y), ImVec2(x + kick_w, kick_y + Px(22)),
                            Lit(kOrange, level), Px(5));
        CenteredText(draw, ImVec2(x + kick_w / 2, kick_y + Px(11)), TextOn(level), kick_names[k]);
    }
    const float height = kick_y + Px(22) - o.y;

    // what the brightness means, and the menu buttons
    const float x = pads_end + Px(36);
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImGui::SetCursorScreenPos(ImVec2(x, o.y));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(right - x, Px(160)));
    ImGui::TextColored(kMuted, velocity
                                   ? "Each hit flashes as brightly as it was hit; the number is "
                                     "its velocity (1 to 127)."
                                   : "An RB1 kit: it doesn't report how hard it's hit, so every "
                                     "hit shows at full strength.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, Px(2)));
    const ImVec2 pills = ImGui::GetCursorScreenPos();
    const float start = Pill(draw, pills, "Start", in.nav.start);
    Pill(draw, ImVec2(pills.x + start + Px(8), pills.y), "Back", in.nav.back);
    ImGui::Dummy(ImVec2(Px(120), Px(24)));
    ImGui::EndGroup();
    const float column_end = ImGui::GetItemRectMax().y;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(right - o.x, std::max(height, column_end - o.y)));
}

void DevicePanel::DrawPad(const input::Gamepad360& g) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    auto held = [&](uint16_t bit) { return (g.buttons & bit) != 0; };

    // triggers and bumpers along the top
    const ImVec2 trigger(Px(90), Px(14));
    Label(draw, o.x, o.y + Px(12), "LT");
    Bar(draw, ImVec2(o.x + Px(30), o.y + Px(5)), trigger, g.left_trigger / 255.0f, kAccent);
    Pill(draw, ImVec2(o.x + Px(132), o.y), "LB", held(xbox::kLeftShoulder));
    const float right_side = o.x + Px(400);
    Pill(draw, ImVec2(right_side - Px(160), o.y), "RB", held(xbox::kRightShoulder));
    Label(draw, right_side - Px(120), o.y + Px(12), "RT");
    Bar(draw, ImVec2(right_side - Px(90), o.y + Px(5)), trigger, g.right_trigger / 255.0f, kAccent);

    const float top = o.y + Px(38);
    // left stick, d-pad
    Stick(draw, ImVec2(o.x + Px(36), top + Px(34)), Px(32), g.thumb_lx, g.thumb_ly,
          held(xbox::kLeftThumb));
    const ImVec2 dpad(o.x + Px(118), top + Px(80));
    const float arm = Px(10), reach = Px(21);
    const std::pair<uint16_t, ImVec2> arms[] = {
        {xbox::kDpadUp, ImVec2(0, -reach)},
        {xbox::kDpadDown, ImVec2(0, reach)},
        {xbox::kDpadLeft, ImVec2(-reach, 0)},
        {xbox::kDpadRight, ImVec2(reach, 0)},
    };
    draw->AddRectFilled(ImVec2(dpad.x - arm, dpad.y - arm), ImVec2(dpad.x + arm, dpad.y + arm),
                        ImGui::GetColorU32(kFrame));
    for (const auto& [bit, offset] : arms) {
        const ImVec2 c(dpad.x + offset.x, dpad.y + offset.y);
        draw->AddRectFilled(ImVec2(c.x - arm, c.y - arm), ImVec2(c.x + arm, c.y + arm),
                            ImGui::GetColorU32(held(bit) ? kAccent : kFrame), Px(3));
    }

    // Back and Start in the middle
    const float back = Pill(draw, ImVec2(o.x + Px(150), top + Px(22)), "Back", held(xbox::kBack));
    Pill(draw, ImVec2(o.x + Px(150) + back + Px(8), top + Px(22)), "Start", held(xbox::kStart));

    // face buttons, right stick
    const ImVec2 face(right_side - Px(44), top + Px(34));
    const float spread = Px(25), button = Px(14);
    const struct {
        uint16_t bit;
        ImVec2 offset;
        ImVec4 color;
        const char* name;
    } faces[] = {
        {xbox::kButtonA, ImVec2(0, spread), kGreen, "A"},
        {xbox::kButtonB, ImVec2(spread, 0), kRed, "B"},
        {xbox::kButtonX, ImVec2(-spread, 0), kBlue, "X"},
        {xbox::kButtonY, ImVec2(0, -spread), kYellow, "Y"},
    };
    for (const auto& f : faces) {
        const ImVec2 c(face.x + f.offset.x, face.y + f.offset.y);
        const float level = held(f.bit) ? 1.0f : 0.0f;
        draw->AddCircleFilled(c, button, Lit(f.color, level));
        CenteredText(draw, c, TextOn(level), f.name);
    }
    Stick(draw, ImVec2(right_side - Px(130), top + Px(80)), Px(32), g.thumb_rx, g.thumb_ry,
          held(xbox::kRightThumb));

    ImGui::Dummy(ImVec2(right_side - o.x, top + Px(114) - o.y));
}

void DevicePanel::DrawMidiHits() {
    namespace midi = input::midi_drums;
    const input::MidiDrumsStatus status = input::GetMidiDrumsStatus();
    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::TextColored(kMuted, "Last notes, newest first");
    if (status.recent.empty()) {
        ImGui::TextColored(kMuted, "Nothing yet: hit a pad.");
        return;
    }
    if (!ImGui::BeginTable("##midi_hits", 3, ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Note");
    ImGui::TableSetupColumn("Velocity");
    ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (auto it = status.recent.rbegin(); it != status.recent.rend() && shown < 6; ++it, ++shown) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u", it->note);
        ImGui::TableNextColumn();
        ImGui::Text("%u", it->velocity);
        ImGui::TableNextColumn();
        if (it->part == midi::Part::kNone) {
            ImGui::TextColored(kMuted, "Not mapped");
        } else if (it->velocity < status.min_velocity) {
            ImGui::TextColored(kMuted, "%s, too quiet", midi::PartName(it->part));
        } else if (it->combo) {
            ImGui::Text("%s, completing the %s combo", midi::PartName(it->part), it->combo);
        } else {
            ImGui::TextUnformatted(midi::PartName(it->part));
        }
    }
    ImGui::EndTable();
}

}
