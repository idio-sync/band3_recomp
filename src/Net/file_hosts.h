#pragma once
#include <optional>
#include <string>
#include <string_view>

// The sites RhythmVerse's uploads are on besides RhythmVerse itself, as far as
// band3 can download from them: a file (not a folder) on MediaFire, Google
// Drive or Dropbox, a song on dingorb (HaramDingo's site, whose files are on
// Google Drive), and a bit.ly or tinyurl link to one of those. Not mega.nz,
// whose files are encrypted, and not an archive (.zip, .rar, .7z), which isn't
// a package band3 can read. Resolve asks the sites (file_hosts_resolve.cpp);
// the rest only reads their links and pages.

namespace band3::file_hosts {

enum class Host { kNone, kRhythmVerse, kMediaFire, kDrive, kDropbox, kDingo, kShortLink };

// the site a link is to, if band3 can download from it; kNone for any other
// (a Drive folder, mega.nz, the Xbox store) and for an archive by its name
Host HostOf(std::string_view url);
inline bool CanDownload(std::string_view url) { return HostOf(url) != Host::kNone; }

// the file ID in a Google Drive link (/file/d/<id>/, ?id=<id>); "" for none
std::string DriveFileId(std::string_view url);
// the file itself, past the warning Drive shows for a file too big to scan
std::string DriveDownloadUrl(std::string_view id);
// that warning page's form, as the URL its button goes to; nullopt for
// another page (the file not shared, or gone)
std::optional<std::string> DriveFormUrl(std::string_view page);

// a Dropbox link that downloads rather than shows the file (dl=1)
std::string DropboxDirect(std::string_view url);

// the download link on a MediaFire file's page; nullopt when it has none
// (the file removed)
std::optional<std::string> MediaFireLink(std::string_view page);

// dingorb's list of its songs (/api/songs) has the song's file on Drive
inline constexpr std::string_view kDingoSongs = "https://www.dingorb.com/api/songs";
std::optional<std::string> DingoDriveUrl(std::string_view songs_json, std::string_view id);

// the first bytes of a file being an Xbox 360 package's (CON, LIVE, PIRS)
bool LooksLikePackage(std::string_view first_bytes);

// The link a song's file itself comes from, asking the site where it must: a
// MediaFire page's download link, a Drive file past its warning for big
// files, where a short link or dingorb leads. "" or why not.
std::string Resolve(const std::string& url, std::string& direct, int depth = 0);

}
