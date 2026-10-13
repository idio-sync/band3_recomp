// A song's envelope read from its package (song_audio.h): the package opened
// as package_songs.cpp opens them, its mogg read whole, its Ogg decoded. A
// song in no package is looked for in the game's archive (the disc's songs);
// an encrypted mogg is decrypted first (mogg_crypt.h).

#include "src/Video/song_audio.h"

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xtypes.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "src/Content/ark_archive.h"
#include "src/Content/package_songs.h"
#include "src/Content/stfs_device.h"
#include "src/Video/mogg.h"
#include "src/Video/mogg_crypt.h"
#include "src/Video/video_decoder.h"
#include "src/config.h"

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

uint32_t Version(std::span<const uint8_t> head) {
    return uint32_t(head[0]) | uint32_t(head[1]) << 8 | uint32_t(head[2]) << 16 |
           uint32_t(head[3]) << 24;
}

// A mogg read whole: its Ogg decoded, decrypted first if it's encrypted
SongAudioResult MoggEnvelope(std::vector<uint8_t>& mogg, const std::string& name,
                             std::vector<float>& envelope, std::string& error) {
    const std::optional<MoggHeader> header = ParseMoggHeader(mogg);
    if (!header) {
        error = name + " isn't a song's audio";
        return SongAudioResult::kFailed;
    }
    if (header->version != kMoggUnencrypted) {
        if (!MoggDecryptable(header->version)) return SongAudioResult::kEncrypted;
        std::string why;
        if (!DecryptMogg(mogg, why)) {
            error = name + " couldn't be decrypted: " + why;
            return SongAudioResult::kFailed;
        }
    }
    envelope =
        OggVorbisEnvelope(std::span(mogg).subspan(header->ogg_offset), kPackageSongSeconds, error);
    return envelope.empty() ? SongAudioResult::kFailed : SongAudioResult::kOk;
}

SongAudioResult FromPackage(const SongMogg& where, std::vector<float>& envelope,
                            std::string& error) {
    const auto device =
        content::OpenPackageDevice("\\Device\\Band3SongAudio\\", rex::to_path(where.package));
    if (!device) {
        error = "its package couldn't be opened";
        return SongAudioResult::kFailed;
    }
    rex::filesystem::Entry* entry = device->ResolvePath(where.mogg);
    if (!entry) {
        error = "its package has no " + where.mogg;
        return SongAudioResult::kFailed;
    }
    if (entry->size() < 8 || entry->size() > kMaxMogg) {
        error = where.mogg + " isn't a song's audio";
        return SongAudioResult::kFailed;
    }
    // the header first: one band3 can't decrypt isn't read further
    uint8_t head[8];
    if (!ReadAll(entry, 0, head)) {
        error = "its audio couldn't be read";
        return SongAudioResult::kFailed;
    }
    const uint32_t version = Version(head);
    if (version != kMoggUnencrypted && !MoggDecryptable(version)) return SongAudioResult::kEncrypted;
    std::vector<uint8_t> mogg(entry->size());
    if (!ReadAll(entry, 0, mogg)) {
        error = "its audio couldn't be read";
        return SongAudioResult::kFailed;
    }
    return MoggEnvelope(mogg, where.mogg, envelope, error);
}

// The game's archives, title update's first (the game takes its files over
// the main one's), each read once and kept while its header's unchanged
std::vector<std::shared_ptr<const content::ArkArchive>> Archives() {
    struct Kept {
        std::filesystem::path header;
        std::filesystem::file_time_type stamp;
        std::shared_ptr<const content::ArkArchive> ark;
    };
    static std::mutex mutex;
    static std::vector<Kept> kept;
    std::lock_guard lock(mutex);
    std::vector<std::shared_ptr<const content::ArkArchive>> out;
    for (const char* name : {"patch_xbox.hdr", "main_xbox.hdr"}) {
        const std::filesystem::path header = GameDataRoot() / "gen" / name;
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(header, ec);
        if (ec) continue;
        auto it = std::find_if(kept.begin(), kept.end(), [&](const Kept& k) { return k.header == header; });
        if (it == kept.end() || it->stamp != stamp) {
            std::string error;
            std::optional<content::ArkArchive> ark = content::ArkArchive::Open(header, error);
            if (!ark) continue;
            if (it == kept.end()) it = kept.insert(kept.end(), Kept{header, stamp, nullptr});
            it->stamp = stamp;
            it->ark = std::make_shared<const content::ArkArchive>(std::move(*ark));
        }
        out.push_back(it->ark);
    }
    return out;
}

// a song of the game's own from its archive: songs/<shortname>/<shortname>.mogg
SongAudioResult FromArchive(std::string_view shortname, std::vector<float>& envelope,
                            std::string& error) {
    const std::string name = std::format("songs/{0}/{0}.mogg", shortname);
    for (const auto& ark : Archives()) {
        if (!ark->Has(name)) continue;
        std::vector<uint8_t> mogg;
        if (!ark->Read(name, mogg, error)) return SongAudioResult::kFailed;
        return MoggEnvelope(mogg, name, envelope, error);
    }
    return SongAudioResult::kNotFound;
}

}

SongAudioResult PackageSongEnvelope(std::string_view shortname, std::vector<float>& envelope,
                                    std::string& error) {
    envelope.clear();
    // on a thread of its own, so it can wait a while for the packages' first read
    const std::optional<SongMogg> where =
        FindSongMogg(content::PackageSongsNow(std::chrono::seconds(20)), shortname);
    if (where) return FromPackage(*where, envelope, error);
    return FromArchive(shortname, envelope, error);
}

}
