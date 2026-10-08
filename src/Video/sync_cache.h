#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/Video/sync_align.h"

// Rhythm envelopes (sync_align.h) kept beside the videos, in their folder's
// .sync: <shortname>.song.env, the song's, captured as it played (the game's
// own audio, which only playing it gives), and <shortname>.video.env, the
// video soundtrack's, stamped with the video's size and time so a changed
// video is read again. They let a video be aligned again (the Videos tab)
// without playing the song.

namespace band3::video {

inline std::filesystem::path SyncFolder(const std::filesystem::path& video) {
    return video.parent_path() / ".sync";
}
inline std::filesystem::path SongEnvelopePath(const std::filesystem::path& video) {
    return SyncFolder(video) / (video.stem().string() + ".song.env");
}
inline std::filesystem::path VideoEnvelopePath(const std::filesystem::path& video) {
    return SyncFolder(video) / (video.stem().string() + ".video.env");
}
// the last alignment's result, for the Videos tab: <shortname>.result
inline std::filesystem::path ResultPath(const std::filesystem::path& video) {
    return SyncFolder(video) / (video.stem().string() + ".result");
}

// a file's size and modification time, as one number; 0 if it isn't there
uint64_t FileStamp(const std::filesystem::path& path);

// saved (its folder made), with `stamp`; false if it can't be
bool SaveEnvelope(const std::filesystem::path& path, std::span<const float> frames,
                  uint64_t stamp);
// the frames, if the file's there, whole, and saved with `stamp` (0: any)
std::optional<std::vector<float>> LoadEnvelope(const std::filesystem::path& path,
                                               uint64_t stamp);

// an alignment's result as text (key = value lines) and back
std::string ResultText(const SyncResult& r);
std::optional<SyncResult> ParseResult(std::string_view text);
bool SaveResult(const std::filesystem::path& path, const SyncResult& r);
std::optional<SyncResult> LoadResult(const std::filesystem::path& path);

}
