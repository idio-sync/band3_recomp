#include "src/Video/video_fetch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <set>
#include <system_error>

#include "src/Video/still_check.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_decoder.h"
#include "src/Video/video_files.h"

namespace band3::video {

namespace {

namespace fs = std::filesystem;

bool Stopped(const FetchSetup& s) { return s.cancel && s.cancel->load(); }

void Stage(const FetchSetup& s, FetchStage stage, double share = 0.0) {
    if (s.progress) s.progress(stage, share);
}

// where downloads are made before they're the song's
fs::path WorkFolder(const FetchSetup& s) { return s.folder / ".downloads"; }

void Remove(const fs::path& p) {
    std::error_code ec;
    if (!p.empty()) fs::remove(p, ec);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<fs::path> Download(const FetchSetup& s, FetchStage stage,
                                 const std::vector<std::string>& args, std::string& error) {
    Stage(s, stage);
    return s.ytdlp->Download(
        args,
        [&](const Progress& p) {
            if (p.total > 0) Stage(s, stage, std::clamp(double(p.done) / double(p.total), 0.0, 1.0));
        },
        error, s.cancel);
}

// a video's sound, fetched, and its rhythm envelope
struct Sound {
    fs::path file;
    std::vector<float> envelope;
};

std::optional<Sound> FetchSound(const FetchSetup& s, const SongInfo& song, std::string_view url,
                                std::string_view tag, std::string& error) {
    std::error_code ec;
    fs::create_directories(WorkFolder(s), ec);
    const fs::path stem = WorkFolder(s) / (song.shortname + "." + std::string(tag) + ".sound");
    std::optional<fs::path> file = Download(s, FetchStage::kFetchingSound, AudioArgs(url, stem), error);
    if (!file) return std::nullopt;
    Sound sound{*file, SoundtrackEnvelope(*file, kVideoSoundtrackSeconds, error)};
    if (sound.envelope.empty()) {
        Remove(sound.file);
        if (error.empty()) error = "its sound couldn't be read";
        return std::nullopt;
    }
    return sound;
}

std::optional<fs::path> FetchPicture(const FetchSetup& s, const SongInfo& song,
                                     std::string_view url, std::string& error) {
    const fs::path stem = WorkFolder(s) / (song.shortname + ".picture");
    std::optional<fs::path> picture =
        Download(s, FetchStage::kFetchingVideo, VideoArgs(url, stem, s.format), error);
    // the picture alone refused (YouTube's 403, now and then): the whole
    // file, picture and sound, is often there
    if (!picture && !Stopped(s)) {
        std::string again;
        picture = Download(s, FetchStage::kFetchingVideo, VideoArgs(url, stem, s.format, true), again);
        if (!picture) error += "; as one file: " + again;
    }
    return picture;
}


// The fetched picture as the song's video, in the videos folder with its
// .ini and sync caches; any other video of the song (another extension)
// goes, so it's this one that's found.
bool Install(const FetchSetup& s, const SongInfo& song, const fs::path& picture, Sound& sound,
             const std::vector<float>* song_env, const std::optional<Match>& match,
             const std::string& source, bool pinned, fs::path& out, std::string& error) {
    Stage(s, FetchStage::kSaving);
    const std::string ext = Lower(picture.extension().string());
    if (std::none_of(std::begin(kVideoExtensions), std::end(kVideoExtensions),
                     [&](std::string_view v) { return v == ext; })) {
        error = "it came as a " + ext + " file, which band3 doesn't play";
        return false;
    }
    std::error_code ec;
    fs::create_directories(s.folder, ec);
    const fs::path video = s.folder / (song.shortname + ext);
    fs::rename(picture, video, ec);
    if (ec) {
        error = "it couldn't be put in the videos folder (" + ec.message() +
                "): is the song's video playing?";
        return false;
    }
    for (std::string_view other : kVideoExtensions) {
        const fs::path p = s.folder / (song.shortname + std::string(other));
        if (p != video) Remove(p);
    }
    if (!sound.envelope.empty()) SaveEnvelope(VideoEnvelopePath(video), sound.envelope, FileStamp(video));
    else Remove(VideoEnvelopePath(video));
    if (song_env) SaveEnvelope(SongEnvelopePath(video), *song_env, 0);
    if (match) SaveResult(ResultPath(video), match->result);
    else Remove(ResultPath(video));
    if (!sound.file.empty()) {
        if (s.keep_audio) {
            fs::create_directories(SyncFolder(video), ec);
            fs::rename(sound.file, SyncFolder(video) / (song.shortname + ".sound" + sound.file.extension().string()),
                       ec);
        }
        Remove(sound.file);
    }
    const auto value = [](bool set, std::string v) {
        return set ? std::optional<std::string>(std::move(v)) : std::nullopt;
    };
    // its offset (a sure map's parts; else one offset), or none to sync as
    // the song first plays
    const bool offset = match && match->result.found;
    if (!WriteIniValues(video, {{"video_source", value(!source.empty(), source)},
                                {"video_pinned", value(pinned, "1")}}) ||
        (offset ? !WriteMatch(video, *match)
                : !WriteIniValues(video, {{"video_start_time", std::nullopt}, {"video_segments", std::nullopt}}))) {
        error = "its .ini couldn't be written";
        return false;
    }
    out = video;
    return true;
}

void Tidy(const FetchSetup& s) {
    std::error_code ec;
    fs::remove(WorkFolder(s), ec);  // only if it's empty
}

std::string Guess(const SyncResult& r) { return std::format("best guess {:+.3f} s", r.offset); }

// "at +1.234 s", or "in 3 parts, the first at +1.234 s"
std::string SyncedAt(const SyncResult& r) {
    return r.parts > 1 ? std::format("in {} parts, the first at {:+.3f} s", r.parts, r.offset)
                       : std::format("at {:+.3f} s", r.offset);
}

// a link or a file of the player's: taken whatever its sound says
FetchResult TakeChosen(const SongInfo& song, const FetchSetup& s, const fs::path& picture,
                       Sound sound, const std::string& source) {
    FetchResult r;
    r.source = source;
    std::vector<float> song_env;
    std::string why;
    const bool have_song = s.song_envelope && s.song_envelope(song, song_env, why);
    std::optional<Match> match;
    if (have_song && !sound.envelope.empty()) {
        Stage(s, FetchStage::kMatching);
        match = MatchSong(song_env, sound.envelope, s.min_overlap);
    }
    const std::optional<double> offset =
        match && match->result.found ? std::optional<double>(match->result.offset) : std::nullopt;
    std::string error;
    if (!Install(s, song, picture, sound, have_song ? &song_env : nullptr, match, source, true, r.video,
                 error)) {
        Remove(picture);
        Remove(sound.file);
        r.end = FetchEnd::kFailed;
        r.message = error;
        return r;
    }
    r.offset = offset;
    if (sound.envelope.empty()) {
        r.end = FetchEnd::kGuessed;
        r.message = "saved; it has no sound to sync by, so set its offset by hand";
    } else if (!have_song) {
        r.end = FetchEnd::kUnsynced;
        r.message = "saved; it syncs the first time its song plays (" + why + ")";
    } else if (match->result.confident) {
        r.end = FetchEnd::kDone;
        r.message = "saved, synced " + SyncedAt(match->result);
    } else {
        r.end = FetchEnd::kGuessed;
        r.message = match->result.found ? "saved, synced by guess (" + Guess(match->result) + "): check it"
                                        : "saved; its sound doesn't match the song's, so set its offset by hand";
    }
    return r;
}

}

std::string_view FetchEndText(FetchEnd end) {
    switch (end) {
        case FetchEnd::kDone: return "Synced";
        case FetchEnd::kGuessed: return "Synced by guess";
        case FetchEnd::kUnsynced: return "Syncs when played";
        case FetchEnd::kReview: return "To review";
        case FetchEnd::kNoVideo: return "No music video found";
        case FetchEnd::kFailed: return "Failed";
        case FetchEnd::kStopped: return "Stopped";
    }
    return "";
}

FetchResult FindSongVideo(const SongInfo& song, const FetchSetup& s) {
    FetchResult r;
    std::vector<float> song_env;
    std::string why;
    const bool have_song = s.song_envelope && s.song_envelope(song, song_env, why);

    Stage(s, FetchStage::kSearching);
    std::string error;
    std::vector<YtCandidate> found = s.ytdlp->Search(SearchQuery(song), 8, error, s.cancel);
    std::vector<ScoredCandidate> ranked = Rank(song, found);
    if (!Stopped(s) && (ranked.empty() || ranked[0].score < kPlausibleScore)) {
        // the official video may not say so
        std::string more_error;
        std::set<std::string> ids;
        for (const YtCandidate& c : found) ids.insert(c.id);
        for (YtCandidate& c : s.ytdlp->Search(PlainQuery(song), 8, more_error, s.cancel))
            if (ids.insert(c.id).second) found.push_back(std::move(c));
        ranked = Rank(song, found);
        if (error.empty()) error = more_error;
    }
    if (Stopped(s)) {
        r.end = FetchEnd::kStopped;
        return r;
    }
    if (found.empty() && !error.empty()) {
        r.end = FetchEnd::kFailed;
        r.message = "the search failed: " + error;
        return r;
    }
    for (ScoredCandidate& c : ranked) r.candidates.push_back({std::move(c), std::nullopt, {}});
    if (r.candidates.empty() || r.candidates[0].candidate.score < kPlausibleScore) {
        r.end = FetchEnd::kNoVideo;
        r.message = "no result looks like its music video";
        return r;
    }

    int tries = 0;
    for (TriedCandidate& t : r.candidates) {
        const ScoredCandidate& c = t.candidate;
        if (c.score < kPlausibleScore || tries >= kMaxTries) break;
        if (!CanAutoAccept(c)) {
            t.problem = c.note.empty() ? "not likely enough to take without asking" : c.note;
            continue;
        }
        tries++;
        error.clear();
        std::optional<Match> matched;
        std::optional<Sound> sound = FetchSound(s, song, c.video.url, c.video.id, error);
        if (Stopped(s)) break;
        if (!sound) {
            t.problem = error;
            continue;
        }
        if (have_song) {
            Stage(s, FetchStage::kMatching);
            matched = MatchSong(song_env, sound->envelope, s.min_overlap);
            t.match = matched->result;
            if (!t.match->found || !t.match->confident) {
                t.problem = t.match->found ? "its sound doesn't surely match the song's (" + Guess(*t.match) + ")"
                                           : "its sound doesn't match the song's";
                Remove(sound->file);
                continue;
            }
        }
        if (!s.auto_accept) {
            t.problem = "for you to choose";
            Remove(sound->file);
            continue;
        }
        std::optional<fs::path> picture = FetchPicture(s, song, c.video.url, error);
        if (Stopped(s)) {
            Remove(sound->file);
            break;
        }
        if (!picture) {
            t.problem = error;
            Remove(sound->file);
            continue;
        }
        Stage(s, FetchStage::kChecking);
        const std::optional<bool> still = IsStillVideo(*picture, c.video.duration, error);
        if (!still || *still) {
            t.problem = still ? "its picture doesn't move (a still picture)"
                              : "its picture couldn't be read: " + error;
            Remove(*picture);
            Remove(sound->file);
            continue;
        }
        const std::optional<double> offset =
            have_song ? std::optional<double>(t.match->offset) : std::nullopt;
        if (!Install(s, song, *picture, *sound, have_song ? &song_env : nullptr, matched, c.video.url,
                     false, r.video, error)) {
            t.problem = error;
            Remove(*picture);
            Remove(sound->file);
            continue;
        }
        r.offset = offset;
        r.source = c.video.url;
        r.end = have_song ? FetchEnd::kDone : FetchEnd::kUnsynced;
        r.message = have_song ? "\"" + c.video.title + "\", synced " + SyncedAt(*t.match)
                              : "\"" + c.video.title + "\"; it syncs the first time its song plays (" +
                                    why + ")";
        Tidy(s);
        return r;
    }
    Tidy(s);
    if (Stopped(s)) {
        r.end = FetchEnd::kStopped;
        return r;
    }
    r.end = FetchEnd::kReview;
    r.message = s.auto_accept ? "nothing sure enough to take: choose one, or paste a link"
                              : "choose one, or paste a link";
    return r;
}

FetchResult FetchSongVideo(const SongInfo& song, std::string_view url, const FetchSetup& s) {
    FetchResult r;
    std::string error;
    std::optional<Sound> sound = FetchSound(s, song, url, "chosen", error);
    std::optional<fs::path> picture;
    if (sound && !Stopped(s)) picture = FetchPicture(s, song, url, error);
    if (Stopped(s) || !sound || !picture) {
        if (sound) Remove(sound->file);
        Tidy(s);
        r.end = Stopped(s) ? FetchEnd::kStopped : FetchEnd::kFailed;
        r.message = error;
        return r;
    }
    r = TakeChosen(song, s, *picture, std::move(*sound), std::string(url));
    Tidy(s);
    return r;
}

FetchResult UseVideoFile(const SongInfo& song, const fs::path& file, const FetchSetup& s) {
    FetchResult r;
    std::error_code ec;
    fs::create_directories(WorkFolder(s), ec);
    const fs::path copy = WorkFolder(s) / (song.shortname + ".file" + file.extension().string());
    fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        r.end = FetchEnd::kFailed;
        r.message = "it couldn't be copied: " + ec.message();
        return r;
    }
    std::string error;
    // its own sound, if it has some
    Sound sound{{}, SoundtrackEnvelope(copy, kVideoSoundtrackSeconds, error)};
    r = TakeChosen(song, s, copy, std::move(sound), {});
    Tidy(s);
    return r;
}

}
