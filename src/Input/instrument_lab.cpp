#include "instrument_lab.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <imgui.h>
#include <rex/cvar.h>
#include "hid_instruments.h"
#include "virtual_instrument.h"
#include "src/settings.h"

namespace band3::input {

namespace {

constexpr const char* kFretNames[kFretCount] = {"Green", "Red", "Yellow", "Blue", "Orange"};
constexpr const char* kPadNames[kPadCount] = {"Red pad", "Yellow pad", "Blue pad", "Green pad"};
constexpr const char* kCymbalNames[kCymbalCount] = {"Yellow cymbal", "Blue cymbal",
                                                    "Green cymbal"};
constexpr const char* kStringNames[kStringCount] = {"Low E", "A", "D", "G", "B", "High E"};
constexpr const char* kNoteNames[12] = {"C", "Db", "D", "Eb", "E", "F",
                                        "Gb", "G", "Ab", "A", "Bb", "B"};
constexpr int kMustangFrets = 17;

// held for as long as the mouse is down on it
bool HoldButton(const char* label) {
    ImGui::Button(label);
    return ImGui::IsItemActive();
}

// true on the frame it is pressed
bool PressButton(const char* label) {
    ImGui::Button(label);
    return ImGui::IsItemActivated();
}

// inputs that only last while a button is held; cleared when the Lab stops
// drawing, so nothing stays stuck down
void ReleaseHeldButtons(InstrumentInputs& in) {
    in.guitar.nav = {};
    in.drums.nav = {};
    in.keys.nav = {};
    in.pro_guitar.nav = {};
    in.guitar.strum_up = false;
    in.guitar.strum_down = false;
    in.keys.overdrive = false;
}

NavInputs DrawNav() {
    NavInputs nav;
    ImGui::TextDisabled("Buttons, held while pressed");
    nav.start = HoldButton("Start");
    ImGui::SameLine();
    nav.back = HoldButton("Back");
    ImGui::SameLine();
    nav.a = HoldButton("A");
    ImGui::SameLine();
    nav.b = HoldButton("B");
    ImGui::SameLine();
    nav.x = HoldButton("X");
    ImGui::SameLine();
    nav.y = HoldButton("Y");
    ImGui::SameLine();
    nav.dpad_up = HoldButton("Up");
    ImGui::SameLine();
    nav.dpad_down = HoldButton("Down");
    ImGui::SameLine();
    nav.dpad_left = HoldButton("Left");
    ImGui::SameLine();
    nav.dpad_right = HoldButton("Right");
    return nav;
}

void DrawGuitar(GuitarInputs& g) {
    ImGui::TextDisabled("Frets");
    for (int f = 0; f < kFretCount; f++) {
        if (f > 0) ImGui::SameLine();
        ImGui::Checkbox(kFretNames[f], &g.frets[f]);
    }
    ImGui::Checkbox("Solo frets", &g.solo);
    g.strum_up = HoldButton("Strum up");
    ImGui::SameLine();
    g.strum_down = HoldButton("Strum down");
    ImGui::SliderFloat("Whammy", &g.whammy, 0.0f, 1.0f);
    ImGui::Checkbox("Tilt", &g.tilt);
    int pickup = g.pickup;
    if (ImGui::SliderInt("Pickup switch", &pickup, 0, 255)) g.pickup = static_cast<uint8_t>(pickup);
}

void DrawDrums(uint8_t velocity) {
    auto& instrument = VirtualInstrument::Get();
    ImGui::TextDisabled("Hits");
    for (int p = 0; p < kPadCount; p++) {
        if (p > 0) ImGui::SameLine();
        if (PressButton(kPadNames[p])) {
            instrument.Pulse([p, velocity](InstrumentInputs& in) { in.drums.pads[p] = velocity; });
        }
    }
    for (int c = 0; c < kCymbalCount; c++) {
        if (c > 0) ImGui::SameLine();
        if (PressButton(kCymbalNames[c])) {
            instrument.Pulse(
                [c, velocity](InstrumentInputs& in) { in.drums.cymbals[c] = velocity; });
        }
    }
    if (PressButton("Kick")) {
        instrument.Pulse([](InstrumentInputs& in) { in.drums.kick1 = true; });
    }
    ImGui::SameLine();
    if (PressButton("Kick 2")) {
        instrument.Pulse([](InstrumentInputs& in) { in.drums.kick2 = true; });
    }
}

void DrawKeys(KeysInputs& k, uint8_t velocity) {
    ImGui::TextDisabled("Keys, click to hold or release");
    for (int key = 0; key < kKeyCount; key++) {
        int note = key % 12;
        if (note != 0) ImGui::SameLine();
        char label[8];
        std::snprintf(label, sizeof(label), "%s%d", kNoteNames[note], key / 12 + 1);
        bool sharp = note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
        if (sharp) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.45f, 0.30f, 0.60f, 1.0f));
        bool held = k.keys[key] != 0;
        if (ImGui::Selectable(label, held, 0, ImVec2(30, 36))) {
            k.keys[key] = held ? 0 : velocity;
        }
        if (sharp) ImGui::PopStyleColor();
    }
    if (ImGui::Button("Release all")) k.keys = {};
    ImGui::SameLine();
    k.overdrive = HoldButton("Overdrive");
}

void DrawProGuitar(ProGuitarInputs& g, InstrumentKind kind, uint8_t velocity) {
    auto& instrument = VirtualInstrument::Get();
    const int max_fret = kind == InstrumentKind::kProGuitarMustang ? kMustangFrets : kMaxProFret;
    ImGui::TextDisabled("Fret held on each string (0 = open), then pluck");
    for (int s = 0; s < kStringCount; s++) {
        ImGui::PushID(s);
        int fret = g.frets[s];
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt(kStringNames[s], &fret, 0, max_fret)) {
            g.frets[s] = static_cast<uint8_t>(fret);
        }
        ImGui::SameLine();
        if (PressButton("Pluck")) {
            instrument.Pulse(
                [s, velocity](InstrumentInputs& in) { in.pro_guitar.velocities[s] = velocity; });
        }
        ImGui::PopID();
    }
    if (PressButton("Strum all")) {
        instrument.Pulse([velocity](InstrumentInputs& in) { in.pro_guitar.velocities.fill(velocity); });
    }
    ImGui::TextDisabled("5-fret colors");
    for (int f = 0; f < kFretCount; f++) {
        if (f > 0) ImGui::SameLine();
        ImGui::PushID(100 + f);
        ImGui::Checkbox(kFretNames[f], &g.colors[f]);
        ImGui::PopID();
    }
    ImGui::Checkbox("Solo", &g.solo);
}

unsigned U16(int16_t v) { return static_cast<uint16_t>(v); }

void DrawCaps(const Caps360& caps) {
    ImGui::TextDisabled("Capabilities");
    ImGui::Text("subtype %u  flags %04X  LX %04X  LY %04X  RX %04X", caps.sub_type, caps.flags,
                U16(caps.gamepad.thumb_lx), U16(caps.gamepad.thumb_ly),
                U16(caps.gamepad.thumb_rx));
}

void DrawState(const Gamepad360& g) {
    ImGui::TextDisabled("Sending");
    ImGui::Text("buttons %04X  LT %02X  RT %02X  LX %04X  LY %04X  RX %04X  RY %04X", g.buttons,
                g.left_trigger, g.right_trigger, U16(g.thumb_lx), U16(g.thumb_ly),
                U16(g.thumb_rx), U16(g.thumb_ry));
}

void DrawReport(InstrumentKind kind) {
    DrawCaps(CapsFor(kind));
    DrawState(Encode(kind, VirtualInstrument::Get().Current()));
}

// a raw report as rows of 16 hex bytes, each row led by its offset
void DrawHex(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        ImGui::TextDisabled("(no reports yet)");
        return;
    }
    char line[96];
    for (size_t row = 0; row < bytes.size(); row += 16) {
        int n = std::snprintf(line, sizeof(line), "%02zu:", row);
        for (size_t i = row; i < bytes.size() && i < row + 16; i++) {
            n += std::snprintf(line + n, sizeof(line) - n, " %02x", bytes[i]);
        }
        ImGui::TextUnformatted(line);
    }
}

void DrawConnectedInstruments() {
    if (!REXCVAR_GET(hid_instruments)) {
        ImGui::TextWrapped("PS3 and Wii Rock Band instruments are read when the hid_instruments "
                           "setting is on (F4, Band3 > Game). It takes a restart.");
        return;
    }
    if (!HidInstrumentsActive()) {
        ImGui::TextWrapped("hid_instruments is on but the HID reader isn't running: restart if "
                           "it was just turned on, otherwise the log says why it didn't start.");
        return;
    }

    const auto statuses = HidInstrumentStatuses();
    if (statuses.empty()) {
        ImGui::TextWrapped("No PS3 or Wii instruments found. Plug in a dongle; it shows up "
                           "within a second or so.");
    }
    for (const auto& status : statuses) {
        char header[160];
        std::snprintf(header, sizeof(header), "%s (%04X:%04X)##%llx", status.name.c_str(),
                      status.vendor, status.product,
                      static_cast<unsigned long long>(status.id));
        if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) continue;

        ImGui::PushID(header);
        ImGui::Text("Reads as %s, release %04X, %llu reports",
                    Ps3InstrumentLabel(status.instrument), status.release,
                    static_cast<unsigned long long>(status.report_count));
        ImGui::TextDisabled("Last report");
        DrawHex(status.last_report);
        DrawCaps(Ps3InstrumentCaps(status.instrument));
        DrawState(status.state);
        if (status.capturing) {
            ImGui::TextUnformatted("Capturing...");
        } else if (ImGui::Button("Save a 5 second capture")) {
            StartHidCapture(status.id, std::chrono::seconds(5));
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    ImGui::Separator();
    const std::string message = LastHidCaptureMessage();
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::TextWrapped("A capture records the raw reports while you play whatever misbehaves, "
                       "into the logs folder next to the executable. Send it along with what "
                       "went wrong.");
}

}

void InstrumentLabDialog::OnDraw(ImGuiIO&) {
    auto& instrument = VirtualInstrument::Get();
    InstrumentInputs in = instrument.Held();

    if (!visible_) {
        ReleaseHeldButtons(in);
        instrument.SetHeld(in);
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    // End pairs with every Begin, whatever Begin returns
    if (!ImGui::Begin("Instrument Lab", &visible_)) {
        ImGui::End();
        ReleaseHeldButtons(in);
        instrument.SetHeld(in);
        return;
    }

    bool virtual_tab = false;
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Virtual instrument")) {
            virtual_tab = true;
            DrawVirtualInstrument(in);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Connected instruments")) {
            DrawConnectedInstruments();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    // held buttons only last while their tab is showing
    if (!virtual_tab) ReleaseHeldButtons(in);
    instrument.SetHeld(in);
    ImGui::End();
}

void InstrumentLabDialog::DrawVirtualInstrument(InstrumentInputs& in) {
    auto& instrument = VirtualInstrument::Get();

    bool connected = REXCVAR_GET(virtual_instrument);
    if (ImGui::Checkbox("Connected", &connected)) {
        rex::cvar::SetFlagByName("virtual_instrument", connected ? "true" : "false");
    }
    ImGui::SameLine();
    int player = REXCVAR_GET(virtual_instrument_player);
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderInt("Player", &player, 1, 4)) {
        rex::cvar::SetFlagByName("virtual_instrument_player", std::to_string(player));
    }

    const InstrumentKind kind = instrument.kind();
    if (ImGui::BeginCombo("Instrument", InstrumentKindLabel(kind))) {
        for (InstrumentKind k : kInstrumentKinds) {
            if (ImGui::Selectable(InstrumentKindLabel(k), k == kind)) instrument.SetKind(k);
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Changing the instrument or player unplugs it for a moment.");
    if (!connected) ImGui::TextDisabled("Not connected; tick Connected to plug it in.");

    ImGui::Separator();
    const NavInputs nav = DrawNav();
    ImGui::Separator();

    if (kind == InstrumentKind::kDrums || kind == InstrumentKind::kKeys ||
        kind == InstrumentKind::kProGuitarMustang || kind == InstrumentKind::kProGuitarSquier) {
        ImGui::SliderInt("Velocity", &velocity_, 1, 127);
    }
    const auto velocity = static_cast<uint8_t>(velocity_);

    switch (kind) {
    case InstrumentKind::kGuitar:
        in.guitar.nav = nav;
        DrawGuitar(in.guitar);
        break;
    case InstrumentKind::kDrums:
        in.drums.nav = nav;
        DrawDrums(velocity);
        break;
    case InstrumentKind::kKeys:
        in.keys.nav = nav;
        DrawKeys(in.keys, velocity);
        break;
    case InstrumentKind::kProGuitarMustang:
    case InstrumentKind::kProGuitarSquier:
        in.pro_guitar.nav = nav;
        DrawProGuitar(in.pro_guitar, kind, velocity);
        break;
    }

    ImGui::Separator();
    DrawReport(kind);
}

}
