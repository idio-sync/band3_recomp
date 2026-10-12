#include "src/Video/video_job_state.h"

#include <cmath>
#include <format>

#include "src/Net/http_request.h"
#include "src/Net/json.h"

namespace band3::video {

namespace {

using http::JsonString;

constexpr int kVersion = 1;

constexpr std::pair<FetchEnd, std::string_view> kEnds[] = {
    {FetchEnd::kDone, "done"},         {FetchEnd::kGuessed, "guessed"},
    {FetchEnd::kUnsynced, "unsynced"}, {FetchEnd::kReview, "review"},
    {FetchEnd::kNoVideo, "no_video"},  {FetchEnd::kFailed, "failed"},
    {FetchEnd::kStopped, "stopped"},
};

std::string_view EndName(FetchEnd end) {
    for (const auto& [e, name] : kEnds)
        if (e == end) return name;
    return "failed";
}

std::optional<FetchEnd> EndOf(std::string_view name) {
    for (const auto& [e, n] : kEnds)
        if (n == name) return e;
    return std::nullopt;
}

std::string Number(double d) { return std::isfinite(d) ? std::format("{}", d) : "0"; }

}

std::string FormatVideoStatuses(const SongVideoStatuses& statuses) {
    std::string out = "{\"version\": " + std::to_string(kVersion) + ", \"songs\": {";
    bool first = true;
    for (const auto& [shortname, s] : statuses) {
        out += first ? "\n  " : ",\n  ";
        first = false;
        out += JsonString(shortname) + ": {\"end\": \"" + std::string(EndName(s.end)) +
               "\", \"message\": " + JsonString(s.message) + ", \"when\": " + std::to_string(s.when) +
               ", \"candidates\": [";
        for (size_t i = 0; i < s.candidates.size(); i++) {
            const TriedCandidate& t = s.candidates[i];
            const YtCandidate& v = t.candidate.video;
            if (i) out += ", ";
            out += "{\"id\": " + JsonString(v.id) + ", \"url\": " + JsonString(v.url) +
                   ", \"title\": " + JsonString(v.title) + ", \"channel\": " + JsonString(v.channel) +
                   ", \"duration\": " + Number(v.duration) + ", \"views\": " + std::to_string(v.views) +
                   ", \"score\": " + Number(t.candidate.score) +
                   ", \"penalized\": " + (t.candidate.penalized ? "true" : "false") +
                   ", \"rejected\": " + (t.candidate.rejected ? "true" : "false") +
                   ", \"note\": " + JsonString(t.candidate.note) + ", \"problem\": " + JsonString(t.problem) +
                   ", \"match\": ";
            if (t.match) {
                out += "{\"offset\": " + Number(t.match->offset) + ", \"score\": " + Number(t.match->score) +
                       ", \"margin\": " + Number(t.match->margin) +
                       ", \"confident\": " + (t.match->confident ? "true" : "false") + "}";
            } else {
                out += "null";
            }
            out += "}";
        }
        out += "]}";
    }
    return out + "\n}}\n";
}

SongVideoStatuses ParseVideoStatuses(std::string_view text) {
    SongVideoStatuses out;
    const auto json = json::Parse(text);
    if (!json || (*json)["version"].Number(0) != kVersion) return out;
    for (const auto& [shortname, s] : (*json)["songs"].Members()) {
        const std::optional<FetchEnd> end = EndOf(s["end"].Text());
        if (shortname.empty() || !end) continue;
        SongVideoStatus status;
        status.end = *end;
        status.message = s["message"].Text();
        status.when = int64_t(s["when"].Number(0));
        for (const json::Value& c : s["candidates"].Items()) {
            TriedCandidate t;
            YtCandidate& v = t.candidate.video;
            v.id = c["id"].Text();
            v.url = c["url"].Text();
            if (v.url.empty()) continue;
            v.title = c["title"].Text();
            v.channel = c["channel"].Text();
            v.duration = c["duration"].Number(0);
            v.views = int64_t(c["views"].Number(-1));
            t.candidate.score = c["score"].Number(0);
            t.candidate.penalized = c["penalized"].Bool();
            t.candidate.rejected = c["rejected"].Bool();
            t.candidate.note = c["note"].Text();
            t.problem = c["problem"].Text();
            if (const json::Value& m = c["match"]; m.IsObject()) {
                SyncResult r;
                r.found = true;
                r.offset = m["offset"].Number(0);
                r.score = m["score"].Number(0);
                r.margin = m["margin"].Number(0);
                r.confident = m["confident"].Bool();
                t.match = r;
            }
            status.candidates.push_back(std::move(t));
        }
        out.emplace(shortname, std::move(status));
    }
    return out;
}

}
