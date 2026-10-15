#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "src/Video/video_fetch.h"

// What finding each song's music video last came to (video_jobs.h), kept in
// the user data root as band3_music_videos.json so the Music Videos screen's review
// list outlasts a restart: {"version": 1, "songs": {"<shortname>": {"end":,
// "message":, "when": seconds since 1970, "candidates": [{"id":, "url":,
// "title":, "channel":, "duration":, "views":, "score":, "penalized":,
// "rejected":, "note":, "problem":, "match": {"offset":, "score":,
// "margin":, "confident":} or null}]}}}.

namespace band3::video {

struct SongVideoStatus {
    FetchEnd end = FetchEnd::kFailed;
    std::string message;
    int64_t when = 0;
    std::vector<TriedCandidate> candidates;
};

using SongVideoStatuses = std::map<std::string, SongVideoStatus, std::less<>>;

std::string FormatVideoStatuses(const SongVideoStatuses& statuses);
// what can't be read is left out; another version's file gives none
SongVideoStatuses ParseVideoStatuses(std::string_view json);

}
