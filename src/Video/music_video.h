#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "src/Video/movie_planes.h"
#include "src/Video/video_files.h"

// Music videos: a song's own video, found by video_files.h, played in the
// video venues in place of their background movies, in time with the song.
// A thread decodes it ahead of the song (video_decoder.h) into the venue
// movie's planes (picture_convert.h); the Movie::Impl::Draw hook
// (src/Hooks/music_video.cpp) asks for the frame to show as it draws.

namespace band3::video {

// the planes' sizes the venue's movie draws: Y w x h, chroma cw x ch
struct PlaneSizes {
    uint32_t w = 0, h = 0, cw = 0, ch = 0;
    bool operator==(const PlaneSizes&) const = default;
};

// whether music videos are on: rb3e_events.cpp then reads each song's
// shortname for SetMusicVideoSong
bool MusicVideosOn();

// whether music_videos_folder has a video for the song (looked for now, on
// the calling thread: a few file checks), for picking a video venue
bool HasMusicVideo(std::string_view shortname);

// black_background_lights: the venue picked for the Black Background modifier
// is a video venue to be drawn black, its band hidden (patches.cpp's
// MetaPerformer::SetVenue sets it at each venue pick, src/Hooks/music_video.cpp
// draws it)
void SetBlackVenue(bool black);
bool BlackVenue();

// For auto-sync (auto_sync.h): the song now playing's video, if it has one,
// and `song`, a number each song changes; and a new start time for that
// song's video, applied from the next frame if it's still on.
std::optional<VideoFile> CurrentMusicVideo(uint64_t& song);
void SetMusicVideoStartTime(uint64_t song, double seconds);

// [ and ] while a video plays (band3_app.h's binds): its start time moved by
// `delta` seconds and saved to its .ini; RecentNudgeMessage says so on
// screen for a couple of seconds after (empty after that)
void NudgeMusicVideo(double delta);
std::string RecentNudgeMessage();

// whether a music video is on screen: MusicVideoFrame gave a venue movie a
// frame of one in the last half second (so a video venue is drawing it), for
// music_video_hide_band
bool MusicVideoShowing();

// The song played, as its Game is made (or GamePanel::Enter enters one a
// setlist plays on in the same Game), empty as it ends: its video is looked
// for, and opened on the player's thread.
void SetMusicVideoSong(std::string_view shortname);

// For the game's thread, as a venue movie draws: the frame to show at
// `song_time` (the song's clock, seconds), at `sizes`. False when there's
// nothing to show instead of the movie: no song, music videos off, or no
// video for the song. While its video isn't decoded that far yet, or the
// song's time is outside it, the frame is black.
bool MusicVideoFrame(double song_time, const PlaneSizes& sizes,
                     std::shared_ptr<const PlaneSet>& out);

}
