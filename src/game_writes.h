#pragma once
#include <filesystem>
#include <string_view>

namespace rex {
class Runtime;
namespace filesystem {
class VirtualFileSystem;
}
}

// game:\ (and d:\) as the game sees them: the game data with a folder in the
// user data root over it. The SDK mounts the game data read-only, so files the
// game writes there (RB3 Deluxe's dx_settings.dta, its playlists, and the rest)
// were dropped. They go to <user_data_root>/game instead, and the game reads
// them back from there; everything else is read from the game data as before.
// A file the game data already has is copied there the first time the game
// writes it (Deluxe's dx_values.dta, which it ships), and the copy is what the
// game sees from then on; the game data itself is never changed.

namespace band3 {

// the folder game:\ writes go to
std::filesystem::path GameWritesFolder(const std::filesystem::path& user_data_root);

// Call after the runtime is set up and before the game starts. Leaves game:\ as
// the SDK mounted it if the folder can't be created.
bool MountGameWrites(rex::Runtime& runtime);
// the same over game_data_root, with writes going to writes_folder
bool MountGameWrites(rex::filesystem::VirtualFileSystem& vfs,
                     const std::filesystem::path& game_data_root,
                     const std::filesystem::path& writes_folder);

// Whether game:\ has a file at path (relative to it, '\' or '/' between
// folders), as the game would open it: in the writes folder or the game data.
// It lists those folders when it mounts them, so a file put in the game data
// while band3 runs isn't there until the next launch. Without the mount, the
// game data alone, as the SDK mounts it.
bool GameFileExists(std::string_view path);

}
