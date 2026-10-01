#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include "src/Input/instrument_kind.h"
#include "game_state.h"

// The test harness's commands: one line of text in, one line of JSON out. They
// act on a TestTarget, the game or a stand-in for it in the unit tests.

namespace band3::test {

struct ScreenshotInfo {
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
};

class TestTarget {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~TestTarget() = default;

    // the virtual instruments, one per player (1-4)
    // the player's instrument, or nothing when none is plugged in
    virtual std::optional<input::InstrumentKind> Kind(int player) = 0;
    // plugs one in, or replugs it as `kind`
    virtual void Plug(int player, input::InstrumentKind kind) = 0;
    virtual void Unplug(int player) = 0;
    virtual input::InstrumentInputs Held(int player) = 0;
    virtual void SetHeld(int player, const input::InstrumentInputs& in) = 0;
    // applies `change` on top of what is held, for `length`
    virtual void Pulse(int player, std::function<void(input::InstrumentInputs&)> change,
                       std::chrono::milliseconds length) = 0;

    virtual GameStateSnapshot State() = 0;
    // an empty name picks one; returns an error, or empty
    virtual std::string Screenshot(const std::string& name, ScreenshotInfo& out) = 0;
    // a Band3 setting only; returns an error, or empty
    virtual std::string SetSetting(std::string_view name, std::string_view value) = 0;
    virtual void Quit() = 0;
    // the harness is shutting down: a wait gives up
    virtual bool Cancelled() = 0;
    // what the game reads from player 1-4, as the input system gives it;
    // false when nothing is connected there
    virtual bool ReadPad(int player, input::Gamepad360& out, uint32_t& packet) = 0;

    virtual Clock::time_point Now() = 0;
    virtual void Sleep(std::chrono::milliseconds length) = 0;
};

struct Condition {
    enum class Kind { kScreen, kScreenContains, kInGame, kMenus, kSong, kFrames };
    Kind kind = Kind::kInGame;
    std::string text;
    uint64_t frames = 0;
};

// a wait condition, or what's wrong with it
std::variant<Condition, std::string> ParseCondition(std::string_view text);
// start_frame: the frame count when the wait began
bool ConditionHolds(const Condition& condition, const GameStateSnapshot& state,
                    uint64_t start_frame);

// runs one command; the reply is one line of JSON, without the newline
std::string RunCommand(std::string_view line, TestTarget& target);

// lets go of everything held on every player, for a client that went away;
// the instruments stay plugged in
void ReleaseAllPlayers(TestTarget& target);

}
