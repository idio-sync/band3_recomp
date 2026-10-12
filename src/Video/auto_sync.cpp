#include "src/Video/auto_sync.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "src/Video/music_video.h"
#include "src/Video/song_audio.h"
#include "src/Video/sync_align.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_decoder.h"
#include "src/Video/video_files.h"
#include "src/settings.h"

namespace band3::video {

namespace {

// the video soundtrack read for the match: the song's start can be this far
// into it (a long intro), plus the captured part
constexpr double kSoundtrackSeconds = 240.0;

std::mutex g_mutex;
// the song (CurrentMusicVideo's count) the capture is for, and what it has
uint64_t g_song = ~0ull;
std::optional<EnvelopeBuilder> g_envelope;
int64_t g_samples = 0;
int g_rate = 0;
bool g_done = false;  // taken (or not wanted) this song

void Align(uint64_t song, VideoFile video, std::vector<float> song_env) {
    SaveEnvelope(SongEnvelopePath(video.path), song_env, 0);
    std::string error;
    const std::optional<SyncResult> result = AlignVideo(video.path, song_env, false, error);
    if (!result) {
        REXLOG_WARN("Music video auto-sync: {}: {}", video.path.string(), error);
        return;
    }
    const SyncResult& r = *result;
    const std::string name = video.path.filename().string();
    if (!r.confident) {
        REXLOG_INFO("Music video auto-sync: {}: best guess {:+.3f} s, not sure enough to save "
                    "(score {:.2f}, margin {:.2f})",
                    name, r.offset, r.score, r.margin);
        return;
    }
    if (video.start_time_set) {
        REXLOG_INFO("Music video auto-sync: {}: {:+.3f} s (score {:.2f}, margin {:.2f}); its .ini "
                    "sets {:+.3f} s, kept",
                    name, r.offset, r.score, r.margin, video.start_time);
        return;
    }
    if (!WriteStartTime(video.path, r.offset)) {
        REXLOG_WARN("Music video auto-sync: {}: {:+.3f} s, but {} couldn't be written", name,
                    r.offset, IniFor(video.path).string());
        return;
    }
    REXLOG_INFO("Music video auto-sync: {}: {:+.3f} s (score {:.2f}, margin {:.2f}), saved to {}",
                name, r.offset, r.score, r.margin, IniFor(video.path).filename().string());
    SetMusicVideoStartTime(song, r.offset);
}

}

namespace {

// the song's sound from its package, kept as a captured one is; false, with
// why in `error`, without it
bool SongFromPackage(const std::filesystem::path& video, std::vector<float>& song,
                     std::string& error) {
    std::string why;
    switch (PackageSongEnvelope(rex::path_to_utf8(video.stem()), song, why)) {
        case SongAudioResult::kOk:
            SaveEnvelope(SongEnvelopePath(video), song, 0);
            return true;
        case SongAudioResult::kNotFound:
            error = "its song isn't in band3's packages (or the game hasn't read them yet): play "
                    "it once with the video to sync it";
            return false;
        case SongAudioResult::kEncrypted:
            error = "its song's audio is encrypted: play it once with the video to sync it";
            return false;
        case SongAudioResult::kFailed:
            error = "its song's audio couldn't be read (" + why +
                    "): play it once with the video to sync it";
            return false;
    }
    return false;
}

}

std::optional<SyncResult> AlignVideo(const std::filesystem::path& video,
                                     std::span<const float> song_env, bool save_offset,
                                     std::string& error) {
    std::vector<float> song;
    if (song_env.empty()) {
        if (std::optional<std::vector<float>> cached = LoadEnvelope(SongEnvelopePath(video), 0))
            song = std::move(*cached);
        else if (!SongFromPackage(video, song, error))
            return std::nullopt;
        song_env = song;
    }
    const uint64_t stamp = FileStamp(video);
    std::vector<float> video_env;
    if (auto cached = LoadEnvelope(VideoEnvelopePath(video), stamp)) {
        video_env = std::move(*cached);
    } else {
        video_env = SoundtrackEnvelope(video, kSoundtrackSeconds, error);
        if (video_env.empty()) return std::nullopt;
        SaveEnvelope(VideoEnvelopePath(video), video_env, stamp);
    }
    const SyncResult r = AlignEnvelopes(song_env, video_env);
    if (!r.found) {
        error = "its sound and the song's don't overlap";
        return std::nullopt;
    }
    SaveResult(ResultPath(video), r);
    if (save_offset && r.confident && !WriteStartTime(video, r.offset)) {
        error = "its .ini couldn't be written";
        return std::nullopt;
    }
    return r;
}

namespace {

// a new song: nothing taken yet, if it wants anything
void Reset(uint64_t song) {
    g_song = song;
    g_envelope.reset();
    g_samples = 0;
    g_rate = 0;
    g_done = false;
}

}

namespace {

// SyncBeforePlaying's thread: the song's sound from its package (or kept from
// before), matched now; unsure or without it, the capture goes on as the
// song plays
void AlignAhead(uint64_t song, VideoFile video) {
    const std::string name = rex::path_to_utf8(video.path.filename());
    std::string error;
    const std::optional<SyncResult> result = AlignVideo(video.path, {}, false, error);
    if (!result) {
        REXLOG_INFO("Music video auto-sync: {}: {}; syncing as it plays", name, error);
        return;
    }
    const SyncResult& r = *result;
    if (!r.confident) {
        REXLOG_INFO("Music video auto-sync: {}: best guess {:+.3f} s from its song's package, not "
                    "sure enough (score {:.2f}, margin {:.2f}); syncing as it plays",
                    name, r.offset, r.score, r.margin);
        return;
    }
    if (!WriteStartTime(video.path, r.offset)) {
        REXLOG_WARN("Music video auto-sync: {}: {:+.3f} s, but {} couldn't be written", name,
                    r.offset, rex::path_to_utf8(IniFor(video.path)));
        return;
    }
    REXLOG_INFO("Music video auto-sync: {}: {:+.3f} s (score {:.2f}, margin {:.2f}) before it "
                "plays, saved to {}",
                name, r.offset, r.score, r.margin, rex::path_to_utf8(IniFor(video.path).filename()));
    // the capture then has nothing to do: the start time's set, the song's sound kept
    SetMusicVideoStartTime(song, r.offset);
}

}

void SyncBeforePlaying() {
    if (!REXCVAR_GET(music_video_auto_sync) || !MusicVideosOn()) return;
    uint64_t song = 0;
    std::optional<VideoFile> video = CurrentMusicVideo(song);
    if (!video || video->start_time_set) return;
    // reading and decoding the song's audio takes a second or two
    std::thread(AlignAhead, song, std::move(*video)).detach();
}

bool SongAudioWanted() {
    if (!REXCVAR_GET(music_video_auto_sync) || !MusicVideosOn()) return false;
    uint64_t song = 0;
    const std::optional<VideoFile> video = CurrentMusicVideo(song);
    std::lock_guard lock(g_mutex);
    if (song != g_song) {
        Reset(song);
        // nothing to take: no video, or one set by hand whose song's sound
        // is kept already
        std::error_code ec;
        g_done = !video || (video->start_time_set &&
                            std::filesystem::exists(SongEnvelopePath(video->path), ec));
    }
    return !g_done;
}

uint64_t SongGeneration() {
    std::lock_guard lock(g_mutex);
    return g_song;
}

void SongAudioBegin(uint64_t generation, int rate) {
    std::lock_guard lock(g_mutex);
    if (generation != g_song || g_done || rate <= 0) return;
    g_envelope.emplace(rate);
    g_rate = rate;
    g_samples = 0;
}

void SongAudio(uint64_t generation, std::span<const float> mono) {
    std::vector<float> envelope;
    {
        std::lock_guard lock(g_mutex);
        if (generation != g_song || g_done || !g_envelope) return;
        g_envelope->Add(mono);
        g_samples += int64_t(mono.size());
        if (g_samples < int64_t(kCaptureSeconds * g_rate)) return;
        envelope = g_envelope->Frames();
        g_envelope.reset();
        g_done = true;
    }
    uint64_t song = 0;
    std::optional<VideoFile> video = CurrentMusicVideo(song);
    if (!video || song != generation) return;
    // decoding the soundtrack takes a second or two: not on the audio thread
    std::thread(Align, song, std::move(*video), std::move(envelope)).detach();
}

}
