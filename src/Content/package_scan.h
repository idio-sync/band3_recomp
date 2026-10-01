// read xbox 360 stfs package headers and find rb3 packages in folders
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace band3::content {

inline constexpr uint32_t kRb3TitleId = 0x45410914;

struct PackageHeader {
    uint32_t magic = 0;          // 'CON ', 'LIVE' or 'PIRS'
    uint32_t content_type = 0;   // 1 saved game (custom songs), 2 marketplace (DLC)
    uint32_t title_id = 0;
    uint32_t license_mask = 0;   // OR of license_bits over licenses with nonzero flags
    std::string content_id;      // the header's 20-byte content ID as 40 uppercase hex digits
    std::u16string display_name; // English display name
};

struct Package {
    std::filesystem::path path;
    PackageHeader header;
};

// enough of a file's start to parse: everything up to the display name's end
inline constexpr size_t kHeaderBytes = 0x511;

std::optional<PackageHeader> ParsePackageHeader(std::span<const uint8_t> bytes);

// every package for title_id in folders and their subfolders, one per content
// ID (the first found); `problems` gets one line per folder that couldn't be
// read, naming the folder
std::vector<Package> ScanFolders(const std::vector<std::filesystem::path>& folders,
                                 uint32_t title_id, std::vector<std::string>* problems);

}
