#include "song_pause.h"

namespace band3::input {

void SongPause::Pause() {
    // a resume not started yet: the song is still paused, so it stays so
    if (goal_ == Goal::kResume && !since_) {
        goal_ = Goal::kNone;
        return;
    }
    Finish();
    goal_ = Goal::kPause;
}

void SongPause::Resume() {
    if (goal_ == Goal::kPause) {
        resume_after_ = true;
        return;
    }
    if (!paused_by_) {
        Finish();
        return;
    }
    goal_ = Goal::kResume;
    since_.reset();
}

void SongPause::Forget() { Finish(); }

void SongPause::Finish() {
    goal_ = Goal::kNone;
    pressing_.reset();
    since_.reset();
    next_ = 0;
    paused_by_.reset();
    resume_after_ = false;
}

std::optional<uint32_t> SongPause::NextPlayer(uint32_t first) const {
    for (uint32_t user = first; user < kPlayers; user++) {
        if (connected_[user]) return user;
    }
    return std::nullopt;
}

bool SongPause::Update(uint32_t user, bool connected, bool gameplay, bool paused,
                       Clock::time_point now) {
    if (user >= kPlayers) return false;
    connected_[user] = connected;
    switch (goal_) {
    case Goal::kNone:
        return false;

    case Goal::kPause:
        if (!gameplay) {
            Finish();
            return false;
        }
        if (paused) {
            // none when it was paused already
            const std::optional<uint32_t> by = pressing_;
            const bool resume = resume_after_ && by;
            Finish();
            paused_by_ = by;
            // the press that paused it has to end before one resumes it
            not_before_ = now + kHold;
            if (resume) goal_ = Goal::kResume;
            return false;
        }
        if (!pressing_) {
            const std::optional<uint32_t> next = NextPlayer(next_);
            if (!next) {
                // no player's Start paused it (or it can't pause yet)
                Finish();
                return false;
            }
            if (*next != user) return false;
            pressing_ = user;
            since_ = now;
        }
        if (*pressing_ != user) return false;
        if (now - *since_ < kHold) return connected;
        if (now - *since_ < kHold + kSettle) return false;
        next_ = *pressing_ + 1;
        pressing_.reset();
        since_.reset();
        return false;

    case Goal::kResume:
        if (!paused || !gameplay || !paused_by_) {
            Finish();
            return false;
        }
        if (user != *paused_by_) return false;
        if (!connected) {
            Finish();
            return false;
        }
        if (now < not_before_) return false;
        if (!since_) since_ = now;
        if (now - *since_ < kHold) return true;
        Finish();
        return false;
    }
    return false;
}

}
