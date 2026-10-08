#pragma once

#include <cstdint>

// Whether a venue the game picks becomes a video venue, so the song's music
// video (music_video.h) shows: music_video_venue_chance percent of the time,
// for a song with a video. A forced venue and the black background modifier
// win, as they win over the game's own picks. Kept apart for unit tests.

namespace band3::video {

struct VideoVenueInputs {
    bool forced_venue = false;      // forced_venue names a venue (it wins)
    bool black_background = false;  // the modifier's no-venue (it wins too)
    bool music_videos = false;      // music_videos on
    bool has_video = false;         // the song's video was found
    int32_t chance = 0;             // music_video_venue_chance, percent
    double roll = 0.0;              // uniform in [0, 1)
};

inline bool PickVideoVenue(const VideoVenueInputs& in) {
    if (in.forced_venue || in.black_background || !in.music_videos || !in.has_video)
        return false;
    return in.roll * 100.0 < double(in.chance);
}

}
