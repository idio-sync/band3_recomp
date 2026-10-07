// read xbox 360 stfs package headers and find rb3 packages in folders
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace band3::content {

inline constexpr uint32_t kRb3TitleId = 0x45410914;
// the titles whose packages RB3 reads: its own, Rock Band's and Rock Band 2's
inline constexpr uint32_t kRb3TitleIds[] = {kRb3TitleId, 0x45410829, 0x45410869};

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

// the end of a file's name while it's still downloading (src/Net/song_downloads.h);
// scans skip it, since its header can be whole while the rest isn't there yet
inline constexpr std::string_view kPartialSuffix = ".part";
// A newer version of the file it's named after (X.pending for X), downloaded
// while the game had X open (src/Net/song_downloads.h): the next scan puts it
// in X's place and keeps X as X.replaced (or X.2.replaced...), which scans
// skip too, so nothing is deleted.
inline constexpr std::string_view kPendingSuffix = ".pending";
inline constexpr std::string_view kReplacedSuffix = ".replaced";

// A package set aside from the web page's Duplicates (src/Net/http_server.cpp):
// X.setaside-next, an empty file beside X, asks for it, since the game may have
// X open; the next scan renames X to X.setaside, which scans skip, and the
// request goes. Nothing is deleted, and putting X back is renaming it back.
inline constexpr std::string_view kSetAsideSuffix = ".setaside";
inline constexpr std::string_view kSetAsideRequestSuffix = ".setaside-next";

// a file a scan leaves out: one still downloading, waiting to replace
// another, replaced, set aside, or asking for that
bool IsSetAside(const std::filesystem::path& path);

// puts pending (X.pending) in X's place, keeping X as X.replaced; "" or why not
std::string ApplyPendingUpdate(const std::filesystem::path& pending);

// asks the next scan to set file (X) aside: writes X.setaside-next; "" or why not
std::string RequestSetAside(const std::filesystem::path& file);
// does what request (X.setaside-next) asks: X becomes X.setaside, and the
// request goes; "" or why not
std::string ApplySetAside(const std::filesystem::path& request);

// a package set aside, or to be at the next scan, by its own name (X)
struct SetAsideFile {
    std::filesystem::path file;
    bool next_launch = false;  // X.setaside-next: asked for, not done yet
};
// every one in folders and their subfolders, by path
std::vector<SetAsideFile> FindSetAside(const std::vector<std::filesystem::path>& folders);
// takes back a request (next_launch), or renames X.setaside back to X; "" or why not
std::string PutBack(const SetAsideFile& set_aside);

std::optional<PackageHeader> ParsePackageHeader(std::span<const uint8_t> bytes);

// the file at path, if it's a package for one of title_ids (and not one set
// aside, IsSetAside)
std::optional<Package> ReadPackage(const std::filesystem::path& path,
                                   std::span<const uint32_t> title_ids);

// every package for one of title_ids in folders and their subfolders (but not
// those set aside, IsSetAside, after pending updates are put in place), one
// per content ID (the first found, in the folders' order); the folders are scanned
// at once, a thread each. `problems` gets one line per folder or subfolder
// that couldn't be read, naming it; the rest of the folder is still read
std::vector<Package> ScanFolders(const std::vector<std::filesystem::path>& folders,
                                 std::span<const uint32_t> title_ids, std::vector<std::string>* problems);

}
