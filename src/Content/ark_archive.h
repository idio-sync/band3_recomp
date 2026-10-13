#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Rock Band 3's archive, read without the game: a header (gen/main_xbox.hdr,
// or the title update's gen/patch_xbox.hdr) listing every file at its offset
// across the archive's .ark parts, which sit beside it. The header is
// encrypted with the DTB cipher (DecryptDtb). As tools/read_game_config.py
// reads it; only version 6, Rock Band 3's.

namespace band3::content {

// The DTB cipher: a 4-byte little-endian seed, then each byte XORed with the
// low byte of a generator it seeds. `data` without its seed, deciphered; the
// same again ciphers it.
std::vector<uint8_t> DecryptDtb(std::span<const uint8_t> data);

class ArkArchive {
public:
    // the archive this header lists; nullopt, with why in `error`, if it can't
    // be read or isn't a version 6 one
    static std::optional<ArkArchive> Open(const std::filesystem::path& header, std::string& error);

    // whether it has this file ("songs/x/x.mogg"), matched without minding case
    bool Has(std::string_view name) const;

    // a file's bytes, whole; false, with why in `error`, if it isn't there
    // or its part can't be read
    bool Read(std::string_view name, std::vector<uint8_t>& out, std::string& error) const;

private:
    struct Entry {
        uint64_t offset;  // across all the parts
        uint32_t size;
    };

    std::filesystem::path folder_;
    std::vector<uint64_t> part_sizes_;
    std::vector<std::string> part_names_;  // their file names in folder_
    std::unordered_map<std::string, Entry> files_;  // by lowercase name
};

}
