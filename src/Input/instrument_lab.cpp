#include "instrument_lab.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <imgui.h>
#include <rex/cvar.h>
#include "hid_instruments.h"
#include "joypad_lag_status.h"
#include "midi_drums_driver.h"
#include "pro_instrument_status.h"
#include "virtual_instrument.h"
#include "src/Audio/mic_mapping_status.h"
#include "src/Audio/usb_mic_capture.h"
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
    bool tilted = g.tilt >= 1.0f;
    if (ImGui::Checkbox("Tilt", &tilted)) g.tilt = tilted ? 1.0f : 0.0f;
    int pickup = g.pickup;
    if (ImGui::SliderInt("Pickup switch", &pickup, 0, 255)) g.pickup = static_cast<uint8_t>(pickup);
}

void DrawDrums(uint8_t velocity) {
    auto& instrument = VirtualInstrument::FromSettings();
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
    auto& instrument = VirtualInstrument::FromSettings();
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
    DrawState(Encode(kind, VirtualInstrument::FromSettings().Current()));
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
                    HidInstrumentTypeLabel(status.instrument), status.release,
                    static_cast<unsigned long long>(status.report_count));
        ImGui::TextDisabled("Last report");
        DrawHex(status.last_report);
        DrawCaps(HidInstrumentCaps(status.instrument));
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

void DrawMidiDrums() {
    if (!REXCVAR_GET(midi_drums)) {
        ImGui::TextWrapped("MIDI drum kits are read when the midi_drums setting is on (F4, "
                           "Band3 > MIDI drums). It takes a restart.");
        return;
    }
    const MidiDrumsStatus status = GetMidiDrumsStatus();
    if (!status.running) {
        ImGui::TextWrapped("midi_drums is on but MIDI input isn't running: restart if it was "
                           "just turned on, otherwise the log says why it didn't start.");
        return;
    }

    if (!status.port.empty()) {
        ImGui::Text("Playing %s", status.port.c_str());
    } else {
        const std::string& wanted = REXCVAR_GET(midi_drums_device);
        ImGui::Text("Waiting for %s", wanted.empty() ? "a MIDI input" : wanted.c_str());
    }
    ImGui::TextDisabled("MIDI inputs");
    if (status.ports.empty()) ImGui::TextUnformatted("(none found)");
    for (const auto& port : status.ports) ImGui::BulletText("%s", port.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Last notes, newest first");
    if (status.recent.empty()) {
        ImGui::TextUnformatted("(nothing yet; hit a pad)");
    } else if (ImGui::BeginTable("notes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Note");
        ImGui::TableSetupColumn("Velocity");
        ImGui::TableSetupColumn("Plays", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto it = status.recent.rbegin(); it != status.recent.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", it->note);
            ImGui::TableNextColumn();
            ImGui::Text("%u", it->velocity);
            ImGui::TableNextColumn();
            if (it->part == midi_drums::Part::kNone) {
                ImGui::TextDisabled("not mapped");
            } else if (it->velocity < status.min_velocity) {
                ImGui::TextDisabled("%s, too quiet", midi_drums::PartName(it->part));
            } else if (it->combo) {
                ImGui::Text("%s, completing the %s combo", midi_drums::PartName(it->part),
                            it->combo);
            } else {
                ImGui::TextUnformatted(midi_drums::PartName(it->part));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("To change what a note plays, set midi_drums_notes (F4), e.g. "
                       "44=Kick,40=Snare. It takes a restart.");
}

// the JoypadTypes whose pro data RB3 reads (UsbMidiGuitar::Poll and
// UsbMidiKeyboard::Poll, for the Xbox types)
bool GameReadsProData(uint32_t game_type) {
    return game_type == 30 || game_type == 31 || game_type == 32 || game_type == 34;
}

void DrawProInstruments() {
    if (!ProPadsPolled()) {
        ImGui::TextWrapped("The game hasn't polled its controllers yet; this fills in once it's "
                           "running.");
        return;
    }
    ImGui::TextWrapped("RB3 reads Pro Keys and Pro Guitar from 16 bytes per player, which band3 "
                       "writes for keytars and pro guitars. The game only reads them for a "
                       "player it sees as one of those, so check that column first.");

    const auto pads = ProPadStatuses();
    if (ImGui::BeginTable("pro", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Instrument");
        ImGui::TableSetupColumn("Game sees");
        ImGui::TableSetupColumn("Bytes band3 wrote", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int pad = 0; pad < kProPads; pad++) {
            const ProPadStatus& s = pads[pad];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", pad + 1);

            ImGui::TableNextColumn();
            if (!s.connected) {
                ImGui::TextDisabled("none");
            } else if (s.subtype == kSubtypeKeytar) {
                ImGui::TextUnformatted("keytar");
            } else if (s.subtype == kSubtypeProGuitar) {
                ImGui::TextUnformatted("pro guitar");
            } else {
                ImGui::Text("subtype %u", s.subtype);
            }

            ImGui::TableNextColumn();
            if (const char* name = JoypadTypeName(s.game_type)) {
                ImGui::Text("%s (%u)", name, s.game_type);
            } else {
                ImGui::Text("type %u", s.game_type);
            }

            ImGui::TableNextColumn();
            if (!s.writing) {
                ImGui::TextDisabled("not written");
                continue;
            }
            char hex[64];
            int n = 0;
            for (uint8_t b : s.data) n += std::snprintf(hex + n, sizeof(hex) - n, "%02x ", b);
            ImGui::TextUnformatted(hex);
            if (!GameReadsProData(s.game_type)) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "the game doesn't read these for this type");
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Bytes 0-9 are the instrument's triggers and sticks. If the game sees the "
                       "right type and the keys or frets are still wrong, send these bytes "
                       "along with what you pressed.");
}

void DrawUsbMics() {
    if (!REXCVAR_GET(usb_mics)) {
        ImGui::TextWrapped("Microphones are used when the usb_mics setting is on (F4, Band3 > "
                           "Microphones). It takes a restart.");
        return;
    }
    const audio::UsbMicStatus status = audio::GetUsbMicStatus();
    if (!status.running) {
        ImGui::TextWrapped("usb_mics is on but recording isn't running: restart if it was just "
                           "turned on, otherwise the log says why it didn't start.");
        return;
    }
    if (status.test_tone > 0) {
        ImGui::Text("Singing a %d Hz test tone into mic slot 1", status.test_tone);
    }

    if (ImGui::BeginTable("mics", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Game thread");
        ImGui::TableSetupColumn("Connected");
        ImGui::TableSetupColumn("Audio fed");
        ImGui::TableHeadersRow();
        for (int i = 0; i < audio::usb_mic::kSlots; i++) {
            const audio::UsbMicSlotStatus& s = status.slots[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            if (s.device.empty()) {
                ImGui::TextDisabled("nothing");
            } else {
                ImGui::TextUnformatted(s.device.c_str());
            }
            ImGui::TableNextColumn();
            if (s.thread) {
                ImGui::TextUnformatted("running");
            } else {
                ImGui::TextDisabled("not started");
            }
            ImGui::TableNextColumn();
            if (s.connected) {
                ImGui::TextUnformatted("yes");
            } else if (s.refusals > 0) {
                ImGui::Text("waiting (%llu tries)", static_cast<unsigned long long>(s.refusals));
            } else {
                ImGui::TextDisabled("no");
            }
            ImGui::TableNextColumn();
            // 16-bit samples
            ImGui::Text("%.1f s", static_cast<double>(s.bytes_fed) /
                                      (audio::usb_mic::kSampleRate * 2));
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Recording devices");
    if (status.test_tone > 0) {
        ImGui::TextUnformatted("(not listed while the test tone plays)");
    } else if (status.devices.empty()) {
        ImGui::TextUnformatted("(none found)");
    }
    for (const auto& device : status.devices) ImGui::BulletText("%s", device.c_str());
    ImGui::TextWrapped("The game starts a thread for each slot along with its audio. Waiting "
                       "means the game hasn't set the slot up yet, which should happen by the "
                       "time vocals are picked; audio fed then climbs while a song plays.");
}

void DrawMicMapping() {
    ImGui::SeparatorText("Which mic each player sings through");
    const audio::MicMapping mapping = audio::GetMicMapping();
    if (!mapping.seen) {
        ImGui::TextWrapped("The game hasn't assigned its mics yet. It does when a mic connects "
                           "or disconnects, or players join.");
        return;
    }
    ImGui::Text("Mics the game has:");
    if (mapping.mics.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("none");
    }
    for (const audio::MicMappingMic& mic : mapping.mics) {
        ImGui::SameLine();
        ImGui::Text(mic.locked ? "%d (held)" : "%d (free)", mic.id);
    }

    if (ImGui::BeginTable("mic_players", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Sings through");
        ImGui::TableSetupColumn("Asked for", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < mapping.players.size(); i++) {
            const audio::MicMappingPlayer& p = mapping.players[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            if (p.actual < 0) {
                ImGui::TextDisabled("no mic");
            } else {
                ImGui::Text("mic %d", p.actual);
            }
            ImGui::TableNextColumn();
            if (p.preferred < 0) {
                ImGui::TextDisabled("any");
            } else {
                ImGui::Text("mic %d", p.preferred);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("These are the game's mic IDs. A mic that is connected and fed but held "
                       "by no player is one the game has but gives to nobody.");
}

void DrawMicrophones() {
    DrawUsbMics();
    DrawMicMapping();
}

void DrawLag() {
    if (!ProPadsPolled()) {
        ImGui::TextWrapped("The game hasn't polled its controllers yet; this fills in once it's "
                           "running.");
        return;
    }
    ImGui::TextWrapped("The extra lag, in ms, RB3 builds in for each player's controller type, "
                       "on top of calibration. band3's instruments reach the game as Xbox ones, "
                       "so they get the Xbox hardware's numbers unless joypad_lag (F4, Band3 > "
                       "Game) changes them, by the type number shown here. The game sets these "
                       "at startup.");

    const auto pads = ProPadStatuses();
    bool overridden = false;
    if (ImGui::BeginTable("lag", 1 + kLagContexts,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Player");
        for (int c = 0; c < kLagContexts; c++) ImGui::TableSetupColumn(LagContextName(c));
        ImGui::TableHeadersRow();
        for (int pad = 0; pad < kProPads; pad++) {
            const ProPadStatus& s = pads[pad];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (const char* name = JoypadTypeName(s.game_type)) {
                ImGui::Text("%d: %s (%u)", pad + 1, name, s.game_type);
            } else {
                ImGui::Text("%d: type %u", pad + 1, s.game_type);
            }
            const auto lag = JoypadLagFor(s.game_type);
            for (int c = 0; c < kLagContexts; c++) {
                ImGui::TableNextColumn();
                if (!lag) {
                    ImGui::TextDisabled("-");
                } else if (lag->used[c] != lag->game[c]) {
                    overridden = true;
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%.0f*", lag->used[c]);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("joypad_lag; the game's is %.0f", lag->game[c]);
                    }
                } else {
                    ImGui::Text("%.0f", lag->used[c]);
                }
            }
        }
        ImGui::EndTable();
    }
    if (overridden) ImGui::TextDisabled("* set by joypad_lag");
}

}

void InstrumentLabDialog::OnDraw(ImGuiIO&) {
    auto& instrument = VirtualInstrument::FromSettings();
    // the Lab's buttons are held only while its Virtual instrument tab shows: let
    // go of them once when it stops showing, and otherwise leave the instrument
    // to whatever else plays it (the test harness)
    auto stop_showing = [&] {
        if (!showing_virtual_) return;
        showing_virtual_ = false;
        InstrumentInputs held = instrument.Held();
        ReleaseHeldButtons(held);
        instrument.SetHeld(held);
    };

    if (!visible_) {
        stop_showing();
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    // End pairs with every Begin, whatever Begin returns
    if (!ImGui::Begin("Instrument Lab", &visible_)) {
        ImGui::End();
        stop_showing();
        return;
    }

    InstrumentInputs in = instrument.Held();
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
        if (ImGui::BeginTabItem("MIDI drums")) {
            DrawMidiDrums();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pro instruments")) {
            DrawProInstruments();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Microphones")) {
            DrawMicrophones();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lag")) {
            DrawLag();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (virtual_tab) {
        showing_virtual_ = true;
        instrument.SetHeld(in);
    } else {
        stop_showing();
    }
    ImGui::End();
}

void InstrumentLabDialog::DrawVirtualInstrument(InstrumentInputs& in) {
    auto& instrument = VirtualInstrument::FromSettings();

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
