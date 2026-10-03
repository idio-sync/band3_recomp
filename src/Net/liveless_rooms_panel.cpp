#include "liveless_rooms_panel.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <imgui.h>
#include <rex/input/input_system.h>
#include "liveless_rooms.h"
#include "online.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/instruments.h"
#include "src/Input/xinput_state.h"
#include "src/settings.h"

namespace band3::rooms {

namespace {

// every letter and digit: which ones a Rooms server makes codes of is its own
// business, and a player with only a controller has to be able to type any
constexpr std::string_view kCodeChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr size_t kCodeLength = 8;
constexpr int kPickerColumns = 9;
constexpr float kKeySize = 34.0f;

const ImVec4 kGood(0.4f, 0.9f, 0.4f, 1.0f);
const ImVec4 kBusy(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kBad(1.0f, 0.4f, 0.4f, 1.0f);
const ImVec4 kInfo(0.4f, 0.8f, 1.0f, 1.0f);

// the pad buttons ImGui's navigation reads; X (delete) and B (close) are the
// panel's own, though B also cancels typing in the field
struct PadKey {
    uint16_t button;
    ImGuiKey key;
};
constexpr PadKey kPadKeys[] = {
    {input::xbox::kDpadUp, ImGuiKey_GamepadDpadUp},
    {input::xbox::kDpadDown, ImGuiKey_GamepadDpadDown},
    {input::xbox::kDpadLeft, ImGuiKey_GamepadDpadLeft},
    {input::xbox::kDpadRight, ImGuiKey_GamepadDpadRight},
    {input::xbox::kButtonA, ImGuiKey_GamepadFaceDown},
    {input::xbox::kButtonB, ImGuiKey_GamepadFaceRight},
};

// the grid's width, which the field and the wrapped text keep to
float PanelWidth() {
    return kPickerColumns * kKeySize + (kPickerColumns - 1) * ImGui::GetStyle().ItemSpacing.x;
}

// IPv4 in network order
std::string Ipv4Text(uint32_t address) {
    uint8_t b[4];
    std::memcpy(b, &address, sizeof(b));
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

ImVec4 StateColor(State state) {
    switch (state) {
        case State::kLoggedIn: return kGood;
        case State::kConnecting:
        case State::kConnected: return kBusy;
        case State::kDisconnected:
        case State::kFailed: return kBad;
        case State::kOff: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

// wrapped at the wrap position OnDraw pushes
void Wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void Hint(const std::string& text) { Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text); }

// letters and digits, as the picker has; CharsUppercase has made the letters
// upper case
int CodeCharFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    return ok ? 0 : 1;
}

// every controller's buttons at once, past the blocker the panel holds.
// Nothing while the game has the input lock: the UI thread doesn't wait on
// the game's threads.
std::optional<uint16_t> ReadPads() {
    rex::input::InputSystem* system = input::GameInputSystem();
    if (!system) return std::nullopt;
    std::unique_lock<std::recursive_mutex> lock(input::InputLock(), std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    using rex::X_RESULT;  // X_ERROR_SUCCESS names it unqualified
    uint16_t buttons = 0;
    for (uint32_t user = 0; user < rex::input::kMaxGuestUsers; user++) {
        rex::input::X_INPUT_STATE state{};
        if (system->GetStateForUI(user, &state) != X_ERROR_SUCCESS) continue;
        // the d-pad and buttons only: instruments put other things in the
        // sticks (drum velocities, whammy)
        buttons |= input::LoadGamepad(state.gamepad).buttons;
    }
    return buttons;
}

}  // namespace

void RoomsPanelDialog::Toggle() {
    if (visible_) {
        visible_ = false;
    } else {
        Show();
    }
}

void RoomsPanelDialog::Show() {
    visible_ = true;
    focus_next_ = true;
}

void RoomsPanelDialog::OnDraw(ImGuiIO& io) {
    // the overshell's Invite Friends, pressed on the game thread
    if (PanelRequested()) Show();
    if (!visible_) {
        ReleasePad(io);
        return;
    }

    // centred each time it opens, as the game's own friends list would be
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (focus_next_) {
        ImGui::SetNextWindowFocus();
        focus_next_ = false;
    }
    // End pairs with every Begin, whatever Begin returns
    if (!ImGui::Begin("Liveless Rooms", &visible_,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ReleasePad(io);
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PanelWidth());
    const Status status = GetStatus();
    if (status.state == State::kOff) {
        DrawOff();
    } else {
        DrawRooms(status);
    }
    ImGui::PopTextWrapPos();

    // the pads are the panel's while it has focus; clicking the game gives
    // them back
    if (visible_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        TakePad(io);
    } else {
        ReleasePad(io);
    }
    ImGui::End();
}

void RoomsPanelDialog::DrawOff() {
    const auto& startup = settings::Startup();
    if (!startup.liveless_rooms) {
        ImGui::TextUnformatted("Liveless Rooms is off.");
        Hint("With it on, players join each other's games by an 8-character code instead "
             "of an address. Under F4 > Band3 > Online, turn on liveless and liveless_rooms, "
             "set username (Band3 > Game) to your name, then restart.");
    } else if (!startup.liveless) {
        Wrapped(kBad, "Liveless Rooms didn't start: it needs liveless.");
        Hint("Turn on liveless under F4 > Band3 > Online, then restart.");
    } else if (!online::IsOwnAccountName(settings::Username())) {
        Wrapped(kBad, "Liveless Rooms didn't start: it needs a username.");
        Hint("Set username (F4 > Band3 > Game) to your own name, not blank or \"User\": "
             "the Rooms server knows you by it. Then restart.");
    } else {
        Wrapped(kBad, "Liveless Rooms didn't start.");
        Hint("The log says why.");
    }
    ImGui::Spacing();
    Hint("B closes this.");
}

void RoomsPanelDialog::DrawRooms(const Status& status) {
    ImGui::TextDisabled("Server");
    ImGui::SameLine(70);
    ImGui::TextUnformatted(status.server.c_str());
    ImGui::TextDisabled("State");
    ImGui::SameLine(70);
    ImGui::TextColored(StateColor(status.state), "%s", std::string(StateName(status.state)).c_str());
    if (status.state == State::kDisconnected || status.state == State::kFailed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect")) {
            requested_code_.clear();
            request_error_ = Connect();
        }
    }
    if (!status.error.empty()) Wrapped(kBad, status.error);

    ImGui::Separator();
    ImGui::TextUnformatted("Your code");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    if (status.code.empty()) {
        ImGui::TextDisabled("--------");
    } else {
        ImGui::TextColored(kInfo, "%s", status.code.c_str());
    }
    ImGui::PopFont();
    Hint("Players join you with it, on UDP port 9103: forward it to this PC, as RB3Enhanced "
         "players do.");
    const int port = settings::Startup().liveless_port;
    if (port != online::kGamePort) {
        Wrapped(kBusy, "liveless_port is " + std::to_string(port) +
                           ", but joins by code go to 9103, so they can't reach this game.");
    }
    if (status.public_ipv4 != 0) {
        Hint("Your public address, as the server sees it: " + Ipv4Text(status.public_ipv4));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Join a game");
    const bool logged_in = status.state == State::kLoggedIn;
    const bool can_join = logged_in && status.game_socket_seen && std::strlen(code_) == kCodeLength;
    ImGui::SetNextItemWidth(PanelWidth());
    if (ImGui::InputTextWithHint("##code", "their code", code_, sizeof(code_),
                                 ImGuiInputTextFlags_CharsUppercase |
                                     ImGuiInputTextFlags_CallbackCharFilter |
                                     ImGuiInputTextFlags_EnterReturnsTrue,
                                 CodeCharFilter) &&
        can_join) {
        TryJoin();
    }
    DrawPicker(can_join);
    if (!status.game_socket_seen) {
        Wrapped(kBusy, "Go online first: Play on Xbox Live in the overshell.");
    } else if (!logged_in) {
        Hint("Joining needs the Rooms server.");
    }
    if (!request_error_.empty()) {
        Wrapped(kBad, request_error_);
    } else if (!requested_code_.empty()) {
        Hint("Asked the server for " + requested_code_ + ".");
    }
    if (!status.last_join_user.empty()) {
        Wrapped(kGood, "Joining " + status.last_join_user + " at " +
                           Ipv4Text(status.last_join_ipv4) + ".");
    }
    Hint("In the host's band, both pick Play Now > Quickplay > Choose Songs.");

    ImGui::Separator();
    Hint("Pad: d-pad moves, A presses, X deletes, B closes.");
}

void RoomsPanelDialog::DrawPicker(bool can_join) {
    const ImVec2 key(kKeySize, kKeySize);
    for (size_t i = 0; i < kCodeChars.size(); i++) {
        if (i % kPickerColumns != 0) ImGui::SameLine();
        const char label[2] = {kCodeChars[i], '\0'};
        if (ImGui::Button(label, key)) AddChar(kCodeChars[i]);
        // where a controller starts: the field would take it into typing
        if (i == 0) ImGui::SetItemDefaultFocus();
    }
    // the bottom row: Backspace three keys wide, Join the rest of the grid's
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    auto wide = [&](int keys) { return ImVec2(keys * kKeySize + (keys - 1) * spacing, kKeySize); };
    if (ImGui::Button("Backspace", wide(3))) Backspace();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_join);
    if (ImGui::Button("Join", wide(kPickerColumns - 3))) TryJoin();
    ImGui::EndDisabled();
}

void RoomsPanelDialog::TryJoin() {
    requested_code_ = code_;
    request_error_ = Join(code_);
}

void RoomsPanelDialog::AddChar(char c) {
    const size_t length = std::strlen(code_);
    if (length >= kCodeLength) return;
    code_[length] = c;
    code_[length + 1] = '\0';
}

void RoomsPanelDialog::Backspace() {
    const size_t length = std::strlen(code_);
    if (length > 0) code_[length - 1] = '\0';
}

void RoomsPanelDialog::TakePad(ImGuiIO& io) {
    rex::input::InputSystem* system = input::GameInputSystem();
    if (!system) return;
    const std::optional<uint16_t> read = ReadPads();
    if (!pad_taken_) {
        system->AddUIInputBlocker();
        pad_taken_ = true;
        // only what isn't on already, so letting go leaves the SDK's as it was
        config_flags_added_ = ImGuiConfigFlags_NavEnableGamepad & ~io.ConfigFlags;
        backend_flags_added_ = ImGuiBackendFlags_HasGamepad & ~io.BackendFlags;
        io.ConfigFlags |= config_flags_added_;
        io.BackendFlags |= backend_flags_added_;
        pad_buttons_ = 0;
        // unread: everything, until a read shows it let go
        pad_ignored_ = read.value_or(0xFFFF);
    }
    if (!read) return;
    pad_ignored_ &= *read;
    const uint16_t buttons = *read & ~pad_ignored_;
    const uint16_t pressed = buttons & ~pad_buttons_;
    // B backs out of typing in the field first; the SDK keeps it from the
    // game until it's let go
    if ((pressed & input::xbox::kButtonB) && !ImGui::IsAnyItemActive()) {
        visible_ = false;
        ReleasePad(io);
        return;
    }
    if (pressed & input::xbox::kButtonX) Backspace();
    for (const PadKey& pad_key : kPadKeys) {
        if ((buttons ^ pad_buttons_) & pad_key.button) {
            io.AddKeyEvent(pad_key.key, (buttons & pad_key.button) != 0);
        }
    }
    pad_buttons_ = buttons;
}

// Not from the destructor: the input system may be gone by then, and the
// process with it.
void RoomsPanelDialog::ReleasePad(ImGuiIO& io) {
    if (!pad_taken_) return;
    for (const PadKey& pad_key : kPadKeys) {
        if (pad_buttons_ & pad_key.button) io.AddKeyEvent(pad_key.key, false);
    }
    io.ConfigFlags &= ~config_flags_added_;
    io.BackendFlags &= ~backend_flags_added_;
    config_flags_added_ = 0;
    backend_flags_added_ = 0;
    pad_buttons_ = 0;
    pad_ignored_ = 0;
    if (rex::input::InputSystem* system = input::GameInputSystem()) {
        system->RemoveUIInputBlocker();
    }
    pad_taken_ = false;
}

}  // namespace band3::rooms
