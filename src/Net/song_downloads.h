#pragma once
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "rhythmverse.h"

// Songs downloaded from RhythmVerse (rhythmverse.h) into the songs folder,
// one at a time on a thread of their own, for the web page's RhythmVerse tab.
// They go into a "rhythmverse" folder in the first of content_folders, and the
// game takes each in once it's in the main hub or the Music Library
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
    kHave,       // already in the content folders (IsDownloaded)
    kUnknown,    // no search has found it
    kNotHosted,  // RhythmVerse doesn't host it (another site, a zip, the official DLC)
    kNoFolder,   // content_folders names no folder
    kNoUpdate,   // asked to update one RhythmVerse has nothing newer of (UpdateOf)
};
// a song, or with update a newer version of one band3 downloaded: it goes
// beside the old one as .pending and takes its place at the next launch
QueueResult QueueDownload(std::string_view file_id, bool update = false);

// what band3 has downloaded, from the download folder's rhythmverse.json
DownloadRecords Records();

// this session's downloads, in the order they were asked for
std::vector<Download> Downloads();

// the files in every content folder and their subfolders, as LocalSongs keeps
// them, however they got there; listed again when a minute old, or after a
// download. A listing with files the last didn't have has the content folders
// scanned again for packages, so the game takes in songs copied in by hand.
std::set<std::pair<std::string, int64_t>> LocalFiles();

// a download in progress stops (and its file goes) when band3 closes
void StopDownloads();

}
