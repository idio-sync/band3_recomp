#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

// band3's pause menu (Escape) pauses a song under it the way a player does:
// with Start, pressed for the game, which opens the song's own pause menu, and
// Start again, from the same player, to resume it. Only a player in the band
// can pause, and nothing says which players those are, so Start goes to each
// connected player in turn until the song pauses. A song already paused when
// the menu opens is left as it is, and so is one paused for some other reason
// when it closes.
//
// This is the timing, kept apart from the SDK so it can be unit tested;
// src/Hooks/song_pause.cpp feeds it the game's reads and adds the presses.
// Not thread safe: the hook locks around it.

namespace band3::input {

class SongPause {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t kPlayers = 4;
    // how long Start is held. The joypad thread reads every 4 ms and keeps a
    // press for the next frame, so this covers a frame of 10 fps.
    static constexpr Clock::duration kHold = std::chrono::milliseconds(100);
    // how long after a press ends the song has to pause before Start goes to
    // the next player
    static constexpr Clock::duration kSettle = std::chrono::milliseconds(400);

    // pause the song, if one is on and isn't paused
    void Pause();
    // resume the song, if Pause paused it and it's still paused. Asked while
    // Pause is still trying, it resumes once the song pauses.
    void Resume();
    // forget Pause: the song stays as it is, and Resume does nothing
    void Forget();

    // For each player the game reads, in order: whether Start is pressed for
    // `user` at `now`. `connected`: the player has a controller. `gameplay`: a
    // song is on screen, where Start pauses it. `paused`: the song is paused.
    bool Update(uint32_t user, bool connected, bool gameplay, bool paused, Clock::time_point now);

private:
    enum class Goal { kNone, kPause, kResume };

    // the lowest connected player from `first` on, as last read
    std::optional<uint32_t> NextPlayer(uint32_t first) const;
    void Finish();

    Goal goal_ = Goal::kNone;
    std::array<bool, kPlayers> connected_{};
    // the player Start is going to, and when it started; none while the next
    // one hasn't been read yet
    std::optional<uint32_t> pressing_;
    std::optional<Clock::time_point> since_;
    // where the search for a player in the band carries on from
    uint32_t next_ = 0;
    // the player whose Start paused the song, and when Start may go to them
    // again to resume it
    std::optional<uint32_t> paused_by_;
    Clock::time_point not_before_{};
    // Resume came while Pause was still trying
    bool resume_after_ = false;
};

}
