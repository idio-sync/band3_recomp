#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "src/Input/instrument_kind.h"
#include "game_state.h"

// The test harness's commands: one line of text in, one line of JSON out. They
// act on a TestTarget, the game or a stand-in for it in the unit tests.

namespace band3::test {

struct ScreenshotInfo {
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    // what drew it: "emulated" (the emulated GPU's picture) or "native"
    std::string renderer;
};

// which picture a screenshot takes: the one the window shows (the renderer
// setting's), or either one whatever it is
enum class ScreenshotSource { kWindow, kEmulated, kNative };

// a screenshot and the native view's capture of the same frame
struct CaptureInfo {
    ScreenshotInfo screenshot;
    std::string capture_path;
    uint64_t frame = 0;  // the native view's frame number
    uint32_t draws = 0;
    // draws left out: shadow-mode draws outside their own shadow pass (0 is
    // expected), and the others (velocity and other unhandled draw modes)
    uint32_t skipped_shadow = 0;
    uint32_t skipped_pass = 0;
    // texture passes: the capture's own and those carried in from earlier
    // frames; render targets its draws sample, those made by a pass whose
    // draws were all left out (rt_filtered: the velocity buffer, or draws
    // with no material or geometry), those no pass it has made otherwise
    // (rt_missing), and native_view_rt_fallback ("guest" or "none")
    uint32_t passes = 0;
    uint32_t passes_carried = 0;
    uint32_t rt_sampled = 0;
    uint32_t rt_missing = 0;
    uint32_t rt_filtered = 0;
    std::string rt_fallback;
    // what the frame drew: ProcCounter's proc_cmds (7 everything; with
    // even/odd rendering 1 the world, 2 post-processing; -1 unknown), whether
    // the capture has the world of the frame before (composed, for a frame
    // that drew none) and the game frame that world is from
    int64_t proc_cmds = -1;
    bool composed = false;
    uint64_t game_frame = 0;
    uint64_t world_frame = 0;
    // no frame whose world the game's picture shows came within the frames
    // the capture waits (CaptureHeldFrame), so it took the last one anyway
    bool held_fallback = false;
    // the native view's GPU backend drawing the same capture, at the
    // screenshot's size: its PNG, or why there's none
    std::string gpu_path;
    std::string gpu_error;
    double gpu_ms = 0;       // the whole GPU frame
    double gpu_wait_ms = 0;  // of that, from submitting it to having the picture
    // the texture passes it drew, and its draws that sampled a render target
    // nothing had drawn (transparent black)
    uint32_t gpu_passes = 0;
    uint32_t gpu_rt_missing = 0;
    // while the native renderer draws the window at another size, the GPU's
    // drawing of the capture at that size too, as it draws the window's
    // (<name>.gpu.presented.png: replay --scale's to check), or empty
    std::string gpu_presented_path;
};

// the live native view (`native_view on`): F9's renderer without its window,
// measured since it was last turned on or off
struct NativeViewStats {
    bool on = false;
    // what drew the last frame, "gpu" or "cpu", and at what size; empty and
    // 0 while it's off
    std::string backend;
    uint32_t width = 0;
    uint32_t height = 0;
    bool post = true;          // with RB3's post-processing
    double seconds = 0;        // since `native_view on`, or `off` while it's off
    uint64_t game_frames = 0;  // the frames the game drew in that time
    uint64_t captured = 0;     // the game's frames the native view captured
    uint64_t rendered = 0;     // of those, the ones it drew
    // captured frames it never drew, because it was still drawing an earlier one
    uint64_t skipped_busy = 0;
    // drawn frames with no world: they drew none and weren't composed with one
    uint64_t worldless = 0;
    // each drawn frame's time; for the GPU the whole frame, uploads and
    // reading back included (GpuStats::ms), and of that from submitting it to
    // having the picture
    std::vector<double> frame_ms;
    std::vector<double> wait_ms;
    // the texture passes recorded while capture was off, in the same time:
    // whether that's on (native_view_record_targets), passes the game drew,
    // those recorded, their draws, and the game thread's time recording them
    bool rt_on = false;
    uint64_t rt_passes = 0;
    uint64_t rt_recorded = 0;
    uint64_t rt_draws = 0;
    double rt_ms = 0;
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
    virtual std::string Screenshot(const std::string& name, ScreenshotSource source,
                                   ScreenshotInfo& out) = 0;
    // a screenshot plus the native view's capture of that same frame, for
    // render checks; an empty name picks one; returns an error, or empty
    virtual std::string Capture(const std::string& name, CaptureInfo& out) = 0;
    // a Band3 setting only; returns an error, or empty
    virtual std::string SetSetting(std::string_view name, std::string_view value) = 0;
    // presses the key a key bind (bind_settings, bind_instrument_lab...) is set
    // to, as the window would, without the window having focus; returns an
    // error, or empty
    virtual std::string PressBind(std::string_view bind) = 0;
    // the live native view, drawing every frame the game captures at width x
    // height as F9's window does (without post-processing unless `post`), and
    // its numbers, which on and off reset; on returns an error, or empty.
    // `sized`: the size was asked for, not the default
    virtual std::string NativeViewOn(uint32_t width, uint32_t height, bool sized, bool post) = 0;
    virtual void NativeViewOff() = 0;
    virtual NativeViewStats NativeView() = 0;
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
    enum class Kind { kScreen, kScreenContains, kInGame, kMenus, kSong, kFrames, kScore, kMic };
    Kind kind = Kind::kInGame;
    std::string text;
    uint64_t frames = 0;
    int64_t score = 0;
    int mic = 0;  // mic slot 1-4
};

// a wait condition, or what's wrong with it
std::variant<Condition, std::string> ParseCondition(std::string_view text);
// start: the state when the wait began
bool ConditionHolds(const Condition& condition, const GameStateSnapshot& state,
                    const GameStateSnapshot& start);

// runs one command; the reply is one line of JSON, without the newline
std::string RunCommand(std::string_view line, TestTarget& target);

// lets go of everything held on every player, for a client that went away;
// the instruments stay plugged in
void ReleaseAllPlayers(TestTarget& target);

}
