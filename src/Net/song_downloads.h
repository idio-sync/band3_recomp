#pragma once
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "rhythmverse.h"

// Songs downloaded from RhythmVerse (rhythmverse.h) into the songs folder,
// one at a time on a thread of their own, for the web page's RhythmVerse tab.
// They go into a "rhythmverse" folder in the first of content_folders, which
// the game reads at its next launch: the folders are scanned once, at startup
// (src/Content/live_content.h). Only songs a search has found can be
// downloaded, and only what RhythmVerse hosts itself; a file that isn't an
// RB3 package is thrown away.

namespace band3::rhythmverse {

// where downloads go; empty when content_folders names no folder
std::filesystem::path DownloadFolder();

// songs a search found, so they can be downloaded
void Remember(const std::vector<Song>& songs);

enum class QueueResult {
    kQueued,     // or already queued, or downloading
    kHave,       // already in the download folder
    kUnknown,    // no search has found it
    kNotHosted,  // RhythmVerse doesn't host it (another site, a zip, the official DLC)
    kNoFolder,   // content_folders names no folder
};
QueueResult QueueDownload(std::string_view file_id);

// this session's downloads, in the order they were asked for
std::vector<Download> Downloads();

// the file IDs of the songs in the download folder
std::set<std::string> DownloadedIds();

// a download in progress stops (and its file goes) when band3 closes
void StopDownloads();

}
