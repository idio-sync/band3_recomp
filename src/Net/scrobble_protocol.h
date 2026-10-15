#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

// What band3 tells Last.fm and ListenBrainz about the songs played
// (src/Net/scrobble.h sends it): when a song counts as listened to, Last.fm's
// signed form parameters, ListenBrainz's JSON, and what their replies mean.

namespace band3::scrobble {

// a song as played: UTF-8 text, and when it started (unix seconds)
struct Track {
    std::string title;
    std::string artist;
    int32_t length_ms = 0;
    int64_t started = 0;
};

// Last.fm's rule, which ListenBrainz follows too: a song longer than 30 s,
// played for half its length or for 4 minutes, whichever comes first
bool ShouldScrobble(int32_t length_ms, int64_t played_s);

// at most this many listens to a scrobble call (Last.fm's limit)
inline constexpr size_t kLastfmBatch = 50;

using Params = std::map<std::string, std::string, std::less<>>;

// api_sig: the MD5 of every parameter's name and value, sorted by name,
// without format and callback, then the shared secret
std::string LastfmSignature(const Params& params, std::string_view secret);

// the parameters as a form, signed (api_sig) and asking for JSON (format)
std::string LastfmForm(Params params, std::string_view secret);

Params LastfmGetToken(std::string_view api_key);
Params LastfmGetSession(std::string_view api_key, std::string_view token);
Params LastfmNowPlaying(const Track& track, std::string_view api_key, std::string_view session);
// the first kLastfmBatch of `tracks`
Params LastfmScrobble(std::span<const Track> tracks, std::string_view api_key,
                      std::string_view session);

// where the browser goes for the player to let band3 scrobble
std::string LastfmAuthUrl(std::string_view api_key, std::string_view token);

enum class Outcome {
    kOk,
    // try again later: no reply, a server error, a rate limit
    kRetry,
    // the session (Last.fm) or token (ListenBrainz) isn't valid: stop until
    // the player signs in again
    kBadCredentials,
    // Last.fm's auth.getSession: the player hasn't allowed band3 yet
    kNotAuthorized,
    // anything else; the listens are dropped, not retried forever
    kRejected,
};

// what a reply to a Last.fm call means, from its HTTP status (0 for none) and
// its JSON body; `message` gets Last.fm's reason, when it gives one
Outcome LastfmOutcome(int status, std::string_view body, std::string* message = nullptr);

// a string member of a Last.fm reply: "token", or "session" then "key"
std::string LastfmToken(std::string_view body);
std::string LastfmSessionKey(std::string_view body);

// a ListenBrainz submit-listens body: "playing_now" for one track (no
// listened_at), "single" for one listen, "import" for more
std::string ListenBrainzJson(std::string_view listen_type, std::span<const Track> tracks);

Outcome ListenBrainzOutcome(int status);

}
