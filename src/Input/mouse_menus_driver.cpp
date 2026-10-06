#include "mouse_menus_driver.h"
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <array>
#include <mutex>
#include <optional>
#include "src/Input/input_system.h"
#include "src/Input/mouse_menus.h"
#include "src/Render/native_view.h"
#include "src/settings.h"

namespace band3::input {

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;
using rex::ui::MouseEvent;

// the window's events arrive on the UI thread, the game reads on its joypad
// thread
struct Shared {
    std::mutex mutex;
    MouseMenus mouse;
    // which players had a controller at their last read
    std::array<bool, kMaxGuestUsers> connected{};
    // the presses last added, and how far the packet numbers have been moved
    uint16_t added = 0;
    uint32_t packets = 0;
};

Shared& State() {
    static Shared shared;
    return shared;
}

std::optional<MouseMenus::Button> ButtonFor(MouseEvent::Button button) {
    switch (button) {
    case MouseEvent::Button::kLeft:
        return MouseMenus::Button::kLeft;
    case MouseEvent::Button::kRight:
        return MouseMenus::Button::kRight;
    default:
        return std::nullopt;
    }
}

// no song, and no band3 dialog holding the controller
bool GameTakesPresses() {
    return REXCVAR_GET(mouse_menus) && !render::InSong() && !GameInputBlocked();
}

class MouseMenusDriver final : public rex::input::InputDriver,
                               public rex::ui::WindowInputListener,
                               public rex::ui::WindowListener {
public:
    MouseMenusDriver() : InputDriver(nullptr, 0) {}
    ~MouseMenusDriver() override {
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
        window->AddInputListener(this, 0);
        window->AddListener(this);
    }

    void OnMouseDown(MouseEvent& e) override {
        const auto button = ButtonFor(e.button());
        if (!button || !TakesClicks()) return;
        std::lock_guard<std::mutex> lock(State().mutex);
        State().mouse.Press(*button);
    }

    // always: a press taken must end, wherever the button comes up
    void OnMouseUp(MouseEvent& e) override {
        const auto button = ButtonFor(e.button());
        if (!button) return;
        std::lock_guard<std::mutex> lock(State().mutex);
        State().mouse.Release(*button);
    }

    void OnMouseWheel(MouseEvent& e) override {
        if (!TakesClicks()) return;
        std::lock_guard<std::mutex> lock(State().mutex);
        State().mouse.Scroll(e.scroll_y(), static_cast<int32_t>(MouseEvent::kScrollPerDetent));
    }

    void OnClosing(rex::ui::UIEvent&) override { Detach(); }

    // a button let go in another window never comes up here
    void OnLostFocus(rex::ui::UISetupEvent&) override {
        std::lock_guard<std::mutex> lock(State().mutex);
        State().mouse.Clear();
    }

private:
    // UI thread. is_active: the window has focus, and none of the SDK's
    // overlays wants the mouse. ImGui wants it over any window band3 or the
    // SDK draws, and whenever a modal dialog is up (the SDK's message boxes
    // and keyboard).
    bool TakesClicks() const {
        if (!GameTakesPresses() || !is_active()) return false;
        return !(ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
    }

    void Detach() {
        if (!window_) return;
        window_->RemoveInputListener(this);
        window_->RemoveListener(this);
        window_ = nullptr;
    }

    // UI thread only
    rex::ui::Window* window_ = nullptr;
};

}

std::unique_ptr<rex::input::InputDriver> CreateMouseMenusDriver() {
    return std::make_unique<MouseMenusDriver>();
}

void AddMouseMenuPresses(uint32_t user, rex::input::X_INPUT_STATE* state) {
    if (user >= kMaxGuestUsers) return;
    Shared& shared = State();
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.connected[user] = state != nullptr;
    if (!state) return;
    // the game reads its players in order each pass, so the lower ones' are
    // this pass's
    for (uint32_t lower = 0; lower < user; lower++) {
        if (shared.connected[lower]) return;
    }

    uint16_t presses = 0;
    if (GameTakesPresses()) {
        presses = shared.mouse.Buttons(MouseMenus::Clock::now());
    } else {
        // a press waiting as a song starts mustn't land once it ends
        shared.mouse.Clear();
    }
    if (presses != shared.added) {
        shared.added = presses;
        shared.packets++;
    }
    state->gamepad.buttons = static_cast<uint16_t>(state->gamepad.buttons | presses);
    state->packet_number = state->packet_number + shared.packets;
}

}
