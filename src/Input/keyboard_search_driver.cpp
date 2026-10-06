#include "keyboard_search_driver.h"
#include <imgui.h>
#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>
#include <atomic>
#include <deque>
#include <mutex>
#include "src/Input/input_system.h"

namespace band3::input {

namespace {

using keyboard_search::Key;
using keyboard_search::Mode;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::ui::KeyEvent;

// Above every other listener on the window, the SDK's keyboard driver (whose
// binds would press buttons) and ImGui among them, whatever their order: it
// yields to ImGui itself (Current).
constexpr size_t kZOrder = 1000;

// the most keys waiting for the game, which takes one a frame as RB3Enhanced
// does; a paste-sized burst fits
constexpr size_t kMaxQueued = 64;

// the game thread sets the mode and takes the keys, the window's events arrive
// on the UI thread
struct Shared {
    std::atomic<Mode> mode{Mode::kOff};
    std::mutex mutex;
    std::deque<Key> keys;
    // the window's, while the driver has one: for the game thread to reach the
    // driver on the UI thread
    std::atomic<rex::ui::WindowedAppContext*> app_context{nullptr};
};

Shared& State() {
    static Shared shared;
    return shared;
}

void Queue(const Key& key) {
    Shared& shared = State();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (shared.keys.size() < kMaxQueued) shared.keys.push_back(key);
}

class KeyboardSearchDriver;
// UI thread only
KeyboardSearchDriver* g_driver = nullptr;
bool g_type_as_focused = false;

// The window sends characters only while it's taking text (SDL's text input),
// and the SDK leaves that off: on, it brings up the input method's windows and
// on some systems an on-screen keyboard (Steam's, on the Deck). So it goes on
// at the first key that types where typing searches, whose own character comes
// from UsCharacter, and off when typing stops searching.
class KeyboardSearchDriver final : public rex::input::InputDriver,
                                   public rex::ui::WindowInputListener,
                                   public rex::ui::WindowListener {
public:
    KeyboardSearchDriver() : InputDriver(nullptr, 0) {}
    ~KeyboardSearchDriver() override {
        if (!window_) return;
        window_->app_context().CallInUIThreadSynchronous([this] { Detach(); });
    }

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    void EnumerateDevices(std::vector<DeviceInfo>&) override {}
    X_RESULT GetDeviceState(DeviceId, rex::input::X_INPUT_STATE*) override {
        return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override {
        return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    X_RESULT SetDeviceVibration(DeviceId, rex::input::X_INPUT_VIBRATION*) override {
        return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
        return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    // the launcher's window, then the runtime's: the same one
    void OnWindowAvailable(rex::ui::Window* window) override {
        if (!window || window == window_) return;
        Detach();
        window_ = window;
        window->AddInputListener(this, kZOrder);
        window->AddListener(this);
        g_driver = this;
        State().app_context.store(&window->app_context());
    }

    void OnKeyDown(KeyEvent& e) override {
        const Mode mode = Current();
        const auto vk = static_cast<uint16_t>(e.virtual_key());
        const keyboard_search::DownAction action = keyboard_search::OnKeyDown(
            mode, vk, e.is_shift_pressed(), e.is_ctrl_pressed(), e.is_alt_pressed());
        if (action.send) Queue(*action.send);
        if (action.types && window_ && !window_->IsTextInputActive()) {
            window_->SetTextInputActive(true);
            typing_ = true;
            // this key's character has gone by
            const std::optional<uint32_t> ch = keyboard_search::UsCharacter(vk, e.is_shift_pressed());
            if (ch && !e.is_ctrl_pressed() && !e.is_alt_pressed()) {
                if (const auto key = keyboard_search::OnChar(mode, *ch, e.is_shift_pressed(), false, false)) {
                    Queue(*key);
                }
            }
        }
        if (action.consume) e.set_handled(true);
    }

    // a character the window took as text; the virtual key is the character
    void OnKeyChar(KeyEvent& e) override {
        const std::optional<Key> key = keyboard_search::OnChar(
            Current(), static_cast<uint32_t>(e.virtual_key()), e.is_shift_pressed(),
            e.is_ctrl_pressed(), e.is_alt_pressed());
        if (!key) return;
        Queue(*key);
        e.set_handled(true);
    }

    // OnKeyUp: never taken, so a bind held as the mode changed comes up

    void OnClosing(rex::ui::UIEvent&) override { Detach(); }

    // UI thread: typing doesn't search any more. Text input stays on if ImGui
    // is typing into one of its windows, which turns it off itself.
    void StopTyping() {
        if (!typing_ || !window_) return;
        typing_ = false;
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
        window_->SetTextInputActive(false);
    }

private:
    // UI thread. The game's mode, while the keys are the game's: no band3
    // dialog holding the controller, the window active (none of the SDK's
    // overlays wants the input), and ImGui not taking the keyboard for one of
    // its windows.
    Mode Current() const {
        if (GameInputBlocked() || !(is_active() || g_type_as_focused)) return Mode::kOff;
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard) return Mode::kOff;
        return State().mode.load(std::memory_order_relaxed);
    }

    void Detach() {
        if (!window_) return;
        StopTyping();
        window_->RemoveInputListener(this);
        window_->RemoveListener(this);
        window_ = nullptr;
        if (g_driver == this) {
            g_driver = nullptr;
            State().app_context.store(nullptr);
        }
    }

    // UI thread only
    rex::ui::Window* window_ = nullptr;
    // text input was turned on here, for typing to search
    bool typing_ = false;
};

}

std::unique_ptr<rex::input::InputDriver> CreateKeyboardSearchDriver() {
    return std::make_unique<KeyboardSearchDriver>();
}

void SetKeyboardSearchMode(Mode mode) {
    Shared& shared = State();
    if (shared.mode.exchange(mode, std::memory_order_relaxed) == mode || mode != Mode::kOff) return;
    // keys typed for a screen that's gone stay out of the next one
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.keys.clear();
    }
    if (auto* context = shared.app_context.load()) {
        context->CallInUIThread([] {
            if (g_driver) g_driver->StopTyping();
        });
    }
}

void TypeAsFocused(bool on) { g_type_as_focused = on; }

std::optional<Key> TakeKeyboardSearchKey() {
    Shared& shared = State();
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (shared.keys.empty()) return std::nullopt;
    const Key key = shared.keys.front();
    shared.keys.pop_front();
    return key;
}

}
