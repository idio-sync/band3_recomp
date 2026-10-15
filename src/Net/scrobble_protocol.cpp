#include "scrobble_protocol.h"

#include <algorithm>
#include <format>
#include "src/Net/http_request.h"
#include "src/Net/json.h"
#include "src/Net/md5.h"
#include "src/Net/rhythmverse.h"

namespace band3::scrobble {

namespace {

// Last.fm's error codes (last.fm/api/errorcodes) that aren't the request's fault
constexpr int kInvalidSession = 9;
constexpr int kServiceOffline = 11;
constexpr int kTokenNotAuthorized = 14;
constexpr int kTemporaryError = 16;
constexpr int kRateLimited = 29;

constexpr std::string_view kClient = "band3_recomp";

}

bool ShouldScrobble(int32_t length_ms, int64_t played_s) {
    if (length_ms <= 30 * 1000) return false;
    return played_s >= 4 * 60 || played_s * 2000 >= length_ms;
}

std::string LastfmSignature(const Params& params, std::string_view secret) {
    std::string text;
    for (const auto& [name, value] : params) {
        if (name == "format" || name == "callback") continue;
        text += name;
        text += value;
    }
    text += secret;
    return net::Md5Hex(text);
}

std::string LastfmForm(Params params, std::string_view secret) {
    params["api_sig"] = LastfmSignature(params, secret);
    params["format"] = "json";
    std::string form;
    for (const auto& [name, value] : params) {
        if (!form.empty()) form += '&';
        form += rhythmverse::FormEncode(name);
        form += '=';
        form += rhythmverse::FormEncode(value);
    }
    return form;
}

Params LastfmGetToken(std::string_view api_key) {
    return {{"method", "auth.getToken"}, {"api_key", std::string(api_key)}};
}

Params LastfmGetSession(std::string_view api_key, std::string_view token) {
    return {{"method", "auth.getSession"},
            {"api_key", std::string(api_key)},
            {"token", std::string(token)}};
}

Params LastfmNowPlaying(const Track& track, std::string_view api_key, std::string_view session) {
    Params params = {{"method", "track.updateNowPlaying"},
                     {"api_key", std::string(api_key)},
                     {"sk", std::string(session)},
                     {"artist", track.artist},
                     {"track", track.title}};
    if (track.length_ms > 0) params["duration"] = std::to_string(track.length_ms / 1000);
    return params;
}

Params LastfmScrobble(std::span<const Track> tracks, std::string_view api_key,
                      std::string_view session) {
    Params params = {{"method", "track.scrobble"},
                     {"api_key", std::string(api_key)},
                     {"sk", std::string(session)}};
    const size_t count = std::min(tracks.size(), kLastfmBatch);
    for (size_t i = 0; i < count; i++) {
        const Track& track = tracks[i];
        params[std::format("artist[{}]", i)] = track.artist;
        params[std::format("track[{}]", i)] = track.title;
        params[std::format("timestamp[{}]", i)] = std::to_string(track.started);
        if (track.length_ms > 0) {
            params[std::format("duration[{}]", i)] = std::to_string(track.length_ms / 1000);
        }
    }
    return params;
}

std::string LastfmAuthUrl(std::string_view api_key, std::string_view token) {
    return std::format("https://www.last.fm/api/auth/?api_key={}&token={}",
                       rhythmverse::FormEncode(api_key), rhythmverse::FormEncode(token));
}

Outcome LastfmOutcome(int status, std::string_view body, std::string* message) {
    const std::optional<json::Value> reply = json::Parse(body);
    const int error = reply ? static_cast<int>((*reply)["error"].Number(0)) : 0;
    if (message && reply) *message = (*reply)["message"].Text();
    if (error == 0) {
        if (status == 200 && reply) return Outcome::kOk;
        return status == 0 || status >= 500 || status == 429 ? Outcome::kRetry : Outcome::kRejected;
    }
    switch (error) {
    case kInvalidSession: return Outcome::kBadCredentials;
    case kTokenNotAuthorized: return Outcome::kNotAuthorized;
    case kServiceOffline:
    case kTemporaryError:
    case kRateLimited: return Outcome::kRetry;
    default: return Outcome::kRejected;
    }
}

std::string LastfmToken(std::string_view body) {
    const std::optional<json::Value> reply = json::Parse(body);
    return reply ? (*reply)["token"].Text() : std::string();
}

std::string LastfmSessionKey(std::string_view body) {
    const std::optional<json::Value> reply = json::Parse(body);
    return reply ? (*reply)["session"]["key"].Text() : std::string();
}

std::string ListenBrainzJson(std::string_view listen_type, std::span<const Track> tracks) {
    const bool playing_now = listen_type == "playing_now";
    std::string json = std::format("{{\"listen_type\":{},\"payload\":[",
                                   http::JsonString(listen_type));
    for (size_t i = 0; i < tracks.size(); i++) {
        const Track& track = tracks[i];
        if (i) json += ',';
        json += '{';
        if (!playing_now) json += std::format("\"listened_at\":{},", track.started);
        json += std::format("\"track_metadata\":{{\"artist_name\":{},\"track_name\":{},"
                            "\"additional_info\":{{",
                            http::JsonString(track.artist), http::JsonString(track.title));
        if (track.length_ms > 0) json += std::format("\"duration_ms\":{},", track.length_ms);
        // closing additional_info, track_metadata and the listen
        json += "\"media_player\":\"Rock Band 3\",\"submission_client\":" +
                http::JsonString(kClient) + "}}}";
    }
    json += "]}";
    return json;
}

Outcome ListenBrainzOutcome(int status) {
    if (status == 200) return Outcome::kOk;
    if (status == 401) return Outcome::kBadCredentials;
    if (status == 0 || status == 429 || status >= 500) return Outcome::kRetry;
    return Outcome::kRejected;
}

}
