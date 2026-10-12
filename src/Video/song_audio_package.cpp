// A song's envelope read from its package (song_audio.h): the package opened
// as package_songs.cpp opens them, its mogg read whole, its Ogg decoded.

#include "src/Video/song_audio.h"

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xtypes.h>

#include <span>

#include "src/Content/package_songs.h"
#include "src/Video/mogg.h"
#include "src/Video/video_decoder.h"

namespace band3::video {

namespace {

// bigger than any song's audio
constexpr size_t kMaxMogg = size_t{1} << 30;

bool ReadAll(rex::filesystem::Entry* entry, size_t offset, std::span<uint8_t> out) {
    rex::filesystem::File* file = nullptr;
    if (!XSUCCEEDED(entry->Open(rex::filesystem::FileAccess::kGenericRead, &file)) || !file)
        return false;
    size_t got = 0;
    const rex::X_STATUS status = file->ReadSync(out, offset, &got);
    file->Destroy();
    return XSUCCEEDED(status) && got == out.size();
}

}

SongAudioResult PackageSongEnvelope(std::string_view shortname, std::vector<float>& envelope,
                                    std::string& error) {
    envelope.clear();
    // on a thread of its own, so it can wait a while for the packages' first read
    const std::optional<SongMogg> where =
        FindSongMogg(content::PackageSongsNow(std::chrono::seconds(20)), shortname);
    if (!where) return SongAudioResult::kNotFound;
    rex::filesystem::StfsContainerDevice device("\\Device\\Band3SongAudio\\",
                                                rex::to_path(where->package));
    if (!device.Initialize()) {
        error = "its package couldn't be opened";
        return SongAudioResult::kFailed;
    }
    rex::filesystem::Entry* entry = device.ResolvePath(where->mogg);
    if (!entry) {
        error = "its package has no " + where->mogg;
        return SongAudioResult::kFailed;
    }
    if (entry->size() < 8 || entry->size() > kMaxMogg) {
        error = where->mogg + " isn't a song's audio";
        return SongAudioResult::kFailed;
    }
    // the header first: an encrypted one isn't read further
    uint8_t head[8];
    if (!ReadAll(entry, 0, head)) {
        error = "its audio couldn't be read";
        return SongAudioResult::kFailed;
    }
    // its version alone: the whole header (its Ogg's offset) points past these 8 bytes
    const uint32_t version = uint32_t(head[0]) | uint32_t(head[1]) << 8 | uint32_t(head[2]) << 16 |
                             uint32_t(head[3]) << 24;
    if (version != kMoggUnencrypted) return SongAudioResult::kEncrypted;
    std::vector<uint8_t> mogg(entry->size());
    if (!ReadAll(entry, 0, mogg)) {
        error = "its audio couldn't be read";
        return SongAudioResult::kFailed;
    }
    const std::optional<MoggHeader> whole = ParseMoggHeader(mogg);
    if (!whole) {
        error = where->mogg + " isn't a song's audio";
        return SongAudioResult::kFailed;
    }
    envelope =
        OggVorbisEnvelope(std::span(mogg).subspan(whole->ogg_offset), kPackageSongSeconds, error);
    return envelope.empty() ? SongAudioResult::kFailed : SongAudioResult::kOk;
}

}
