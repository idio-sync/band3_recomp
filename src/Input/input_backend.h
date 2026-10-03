#pragma once
#include <string_view>

namespace band3::input {

// The backend the SDK's CreateDefaultInputSystem builds for an input_backend
// value: XInput only for exactly "xinput" on Windows (it compares case
// sensitively), SDL for anything else ("SDL", a typo) and always off Windows.
constexpr std::string_view BackendFor(std::string_view value, bool windows) {
    return windows && value == "xinput" ? "xinput" : "sdl";
}

}
