# Folders, Live Content and Song IDs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make band3's folders configurable from the ini (portable installs), let RB3 read DLC and custom songs straight from folders without unpacking, and correct custom songs' text `song_id`s as RB3Enhanced does.

**Architecture:** Pure, SDK-free logic (path resolution, STFS header parsing, folder scanning, CRC song IDs) lives in small files the unit tests build. The SDK-facing parts are thin: `Band3App::OnConfigurePaths` for folders; an override of RB3's `XContentCreateCrossTitleEnumerator` (where it lists DLC and custom songs) plus overrides of three XAM exports (`XamContentCreateEx`, `XamContentClose`, `XamContentGetCreator`) that handle band3's packages and call the SDK's own for everything else; the existing `DataNode__Int` override and a `GetSongID` override for song IDs.

**Tech Stack:** C++23, clang via Ninja, ReXGlue SDK 0.10 (prebuilt in `.rexglue-sdk`), doctest unit tests (`tests/`), the game test harness (`tools/band3ctl.py`, `.b3t` scripts).

**Spec:** `docs/superpowers/specs/2026-10-01-folders-and-live-content-design.md`

## Global Constraints

- Code style: 4-space indent, lowercase terse comments in the codebase's voice, `namespace band3::...`, match neighbouring files. No new third-party dependencies.
- band3 builds on Windows and Linux; CI (`.github/workflows/ci.yml`) runs the unit tests on both and compile-checks every `src/*.cpp` against the SDK on both. Windows-only code goes under `#ifdef _WIN32`, with the feature logged as unavailable elsewhere. Unit tests must pass on both (no drive-letter-only assumptions).
- Unit-tested code must not include any `<rex/...>` header (the tests build without the SDK); list every new test file and every `src/` file it needs in `tests/CMakeLists.txt`.
- Command line wins over `band3.toml`, which wins over `band3_config.ini` (existing rule; see `src/config.cpp` `ApplyLegacyIni`).
- A relative path in the ini, or in `content_folders`, is relative to the folder `band3_config.ini` was found in (its "anchor"); with no ini found, the working directory.
- Folder lists separate folders with `|` (no path can contain it, and the ini reader treats ` ;` as the start of a comment).
- Paths that reach logs or narrow-string APIs go through `rex::path_to_utf8` / `rex::to_path`, never `path::string()` (it throws on Windows for characters outside the ANSI code page).
- RB3's title ID is `0x45410914`.
- Song ID correction: `crc32(text) % 9999999 + 2130000000`, standard CRC-32 (zlib's), the same as RB3Enhanced.
- Never write to `assets/` or to a content folder.
- Test launches start minimized and never take focus: use `tools/band3ctl.py launch` (it does), or `SW_SHOWMINNOACTIVE` if starting `band3.exe` another way.
- Commit after each task with the repo's message style (imperative summary, explanatory body) ending in `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Git has no identity configured: commit with `git -c user.name=idio-sync -c user.email=jedivoodoo@gmail.com commit ...`.

### Building and testing (from the worktree root, PowerShell)

Neither clang nor Ninja is on the shell's `PATH`, and a configure without them resets the build folder. Run every cmake command through `out\b3env.cmd`, which sets the environment up:

```powershell
cmd /c "out\b3env.cmd cmake --build out/build/win-amd64-release"
cmd /c "out\b3env.cmd cmake --preset win-amd64-release"            # after adding a src/*.cpp (the source list is a glob)
cmd /c "out\b3env.cmd cmake --build out/tests" ; cmd /c "out\b3env.cmd ctest --test-dir out/tests --output-on-failure"
```

`out/tests` is already configured. Never run `cmake --preset` from a bare shell. Every build runs codegen, which rewrites `band3_manifest.toml` with LF line endings: run `git checkout -- band3_manifest.toml` before committing (never commit that file). Changing `band3_config.toml` needs codegen first: `.\.rexglue-sdk\bin\rexglue.exe codegen band3_manifest.toml` (~2 minutes; it rewrites `band3_manifest.toml` with LF endings: `git checkout -- band3_manifest.toml` afterwards if that's its only change). No task in this plan needs codegen.

Game runs: `python tools/band3ctl.py launch --fresh --user-data out/<name> -- --log_file=<absolute path>.log` then `python tools/band3ctl.py run tests/game/boot.b3t` to reach the main menu on Rock Band 3 Deluxe (`assets/default.xex` is Deluxe). `python tools/band3ctl.py quit` when done. The HTTP server (`-- --http_enabled=true`, port 21070) answers `GET /list_songs` (every song's shortname, title, artist) and `GET /song_<id>` (one song by ID): use it to prove songs are in the library. Before launching, make sure no other `band3.exe` is running (`tasklist /FI "IMAGENAME eq band3.exe"`).

Sample packages (custom songs, all `CON`, content type 1, title `45410914`) are on `\\BISHOP\Downloads\rb`. Never modify or delete them; copy the ones a task names into `out/test_songs/`.

## Review Focus

1. **Saves after the XAM overrides:** the profile and Deluxe's save must still create and load (they go through the same `XamContentCreateEx`), and custom songs must not show up in RB3's save or song-cache searches (`XamContentCreateEnumerator`, type 1). Tasks 3 and 4 relaunch with the same user data and check the profile loads.
2. **A content folder that's missing or unreachable** (a NAS that's off): startup isn't held up, a warning names the folder, no crash. Task 2 tests a missing folder; Task 4 sets a nonexistent one and an unreachable UNC path in the game and reports startup time.
3. **Reading a package while the game reads it elsewhere:** RB3 streams a song's audio while it loads other files from the same package, and the SDK's STFS file reads share one `FILE*` per package without a lock (`StfsContainerFile::ReadSync`). Task 4 plays a whole song and uses the Music Library's previews; glitches mean stop and report (an SDK limitation to fix there).
4. **`user_data_root` set in the ini without `cache_root`:** the cache follows the user data root rather than staying in `Documents\band3\cache`. Task 1's portable launch checks `user_data\cache` exists.
5. **Odd file names and paths:** 81-character names, spaces, `+`, `(`, `'`, non-ASCII folder names: the package's name to the game is its content ID, not its file name, and paths never go through `path::string()`. Task 4 uses the 81-character Death Cab file; Task 1 runs from a folder with a non-ASCII name.

---

### Task 1: Folders from the ini (portability)

**Files:**
- Create: `src/paths.h`, `src/paths.cpp`
- Create: `tests/paths_test.cpp`
- Modify: `tests/CMakeLists.txt` (add `paths_test.cpp` and `${BAND3_ROOT}/src/paths.cpp`)
- Modify: `src/config.h`, `src/config.cpp` (find the ini; read path keys; `GameDataRoot` becomes a path)
- Modify: `src/Hooks/file.cpp:44`, `src/Net/http_server.cpp:120` (`GameDataRoot()` callers, if the type change needs it)
- Modify: `src/band3_app.h` (`OnConfigurePaths`, `OnPostInitLogging`)
- Modify: `band3_config.ini` (`[game]` documents the new keys)
- Modify: `README.md` (Settings section)

**Interfaces:**
- Produces (in `src/paths.h`, namespace `band3::paths`, no SDK includes):
  - `std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);` empty → empty path; absolute → itself; relative → `(anchor / value).lexically_normal()`. `value` is UTF-8.
  - `std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs, std::string_view name);` first `dir / name` that is a regular file, else empty.
  - `std::vector<std::string> SplitList(std::string_view value);` splits on `|`, trims spaces and tabs, drops empty parts.
- Produces (in `src/config.h`, namespace `band3`):
  - `const std::filesystem::path& LegacyIniPath();` the ini found (working directory, then beside the exe), or `kLegacyIniPath` relative if neither has one. Found once, then cached.
  - `std::filesystem::path IniAnchor();` the folder `LegacyIniPath()` is in, absolute; the working directory if no ini was found.
  - `std::string ReadIniString(const char* key);` `[game] key` from the ini, unquoted (empty if missing).
  - `const std::filesystem::path& GameDataRoot();` / `void SetGameDataRoot(std::filesystem::path root);` (was `std::string`).

- [ ] **Step 1: Write the failing tests** — `tests/paths_test.cpp`:

```cpp
// Checks band3's folder settings (src/paths.cpp): how a configured path is
// resolved against the ini's folder, finding the ini, and folder lists.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "src/paths.h"

namespace fs = std::filesystem;
using namespace band3::paths;

TEST_CASE("a relative path is relative to the anchor") {
    const fs::path anchor = fs::temp_directory_path() / "band3";
    CHECK(Resolve("user_data", anchor) == (anchor / "user_data").lexically_normal());
    CHECK(Resolve("../shared/songs", anchor) == (anchor / ".." / "shared" / "songs").lexically_normal());
}

TEST_CASE("an absolute path is kept and an empty one stays empty") {
    const fs::path anchor = fs::temp_directory_path() / "band3";
    const fs::path absolute = fs::temp_directory_path() / "rb" / "songs";
    CHECK(Resolve(absolute.string(), anchor) == absolute);
    CHECK(Resolve("//BISHOP/Downloads/rb", anchor) == fs::path("//BISHOP/Downloads/rb"));
#ifdef _WIN32
    CHECK(Resolve("D:/rb/songs", anchor) == fs::path("D:/rb/songs"));
#else
    CHECK(Resolve("/srv/rb/songs", anchor) == fs::path("/srv/rb/songs"));
#endif
    CHECK(Resolve("", anchor).empty());
}

TEST_CASE("the first folder holding the file wins") {
    const fs::path root = fs::temp_directory_path() / "band3_paths_test";
    fs::remove_all(root);
    fs::create_directories(root / "cwd");
    fs::create_directories(root / "exe");
    std::ofstream(root / "exe" / "band3_config.ini") << "[game]\n";
    CHECK(FindFile({root / "cwd", root / "exe"}, "band3_config.ini") == root / "exe" / "band3_config.ini");
    std::ofstream(root / "cwd" / "band3_config.ini") << "[game]\n";
    CHECK(FindFile({root / "cwd", root / "exe"}, "band3_config.ini") == root / "cwd" / "band3_config.ini");
    CHECK(FindFile({root / "missing"}, "band3_config.ini").empty());
    // a folder of that name isn't the file
    fs::create_directories(root / "dir" / "band3_config.ini");
    CHECK(FindFile({root / "dir"}, "band3_config.ini").empty());
    fs::remove_all(root);
}

TEST_CASE("a folder list splits on bars and drops blanks") {
    CHECK(SplitList("").empty());
    CHECK(SplitList(" | |").empty());
    CHECK(SplitList("songs") == std::vector<std::string>{"songs"});
    CHECK(SplitList(" songs | \\\\BISHOP\\Downloads\\rb |D:/dlc") ==
          std::vector<std::string>{"songs", "\\\\BISHOP\\Downloads\\rb", "D:/dlc"});
    // spaces and semicolons inside a folder's name are kept
    CHECK(SplitList("My Songs|a;b") == std::vector<std::string>{"My Songs", "a;b"});
}
```

Add both files to `tests/CMakeLists.txt`'s `add_executable(band3_tests ...)` list (test files with the other tests, `${BAND3_ROOT}/src/paths.cpp` with the other sources).

- [ ] **Step 2: Run the tests to see them fail** (build error: `src/paths.h` not found). `cmake --build out/tests` reconfigures itself for the CMakeLists change.

- [ ] **Step 3: Implement `src/paths.h` / `src/paths.cpp`**

```cpp
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// band3's folder settings: where its folders are, and the folders it reads
// songs from. A relative path is relative to the folder band3_config.ini is
// in, so a whole install can move as one folder.

namespace band3::paths {

// value (UTF-8) resolved against anchor; an empty value stays empty
std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);

// the first dir / name that is a file, or empty
std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name);

// "a| b ||c" -> {"a", "b", "c"}
std::vector<std::string> SplitList(std::string_view value);

}
```

```cpp
#include "paths.h"
#include <system_error>

namespace band3::paths {

namespace {

std::filesystem::path FromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}

std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor) {
    if (value.empty()) return {};
    auto path = FromUtf8(value);
    if (path.is_absolute()) return path;
    return (anchor / path).lexically_normal();
}

std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name) {
    std::error_code ec;
    for (const auto& dir : dirs) {
        auto candidate = dir / FromUtf8(name);
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<std::string> SplitList(std::string_view value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('|', start);
        if (end == std::string_view::npos) end = value.size();
        auto part = value.substr(start, end - start);
        const auto first = part.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const auto last = part.find_last_not_of(" \t");
            parts.emplace_back(part.substr(first, last - first + 1));
        }
        start = end + 1;
    }
    return parts;
}

}
```

(`std::u8string(value.begin(), value.end())` converts `char` to `char8_t` element-wise; if clang rejects the iterator-pair constructor across types, build it with `reinterpret_cast<const char8_t*>(value.data()), value.size()`.)

- [ ] **Step 4: Run the tests to see them pass.**

- [ ] **Step 5: Wire it into config and the app.**

`src/config.h`: add the **Interfaces** declarations (include `<filesystem>`), and change `GameDataRoot`/`SetGameDataRoot` to `std::filesystem::path`. `src/config.cpp`:

```cpp
#include <fstream>
#include <sstream>
#include <rex/filesystem.h>   // GetExecutableFolder, path_to_utf8
#include "paths.h"

namespace {

// read through a path, not a narrow file name, so a non-ASCII folder works
INIReader ReadIni() {
    std::ifstream file(LegacyIniPath(), std::ios::binary);
    if (!file) return INIReader(std::string());   // ParseError() == -1 for a missing file is checked below via the path
    std::stringstream text;
    text << file.rdbuf();
    const std::string s = text.str();
    return INIReader(s.c_str(), s.size());
}

}

const std::filesystem::path& LegacyIniPath() {
    static const std::filesystem::path path = [] {
        std::error_code ec;
        auto found = paths::FindFile(
            {std::filesystem::current_path(ec), rex::filesystem::GetExecutableFolder()},
            kLegacyIniPath);
        return found.empty() ? std::filesystem::path(kLegacyIniPath) : found;
    }();
    return path;
}

std::filesystem::path IniAnchor() {
    std::error_code ec;
    auto path = std::filesystem::absolute(LegacyIniPath(), ec);
    return std::filesystem::is_regular_file(path, ec) ? path.parent_path()
                                                       : std::filesystem::current_path(ec);
}

std::string ReadIniString(const char* key) {
    return Unquote(ReadIni().Get("game", key, ""));
}
```

Check `src/ThirdParty/inih/INIReader.h` for the exact buffer constructor (`INIReader(const char* buffer, size_t buffer_size)`) and how `ParseError()` reports a missing file; keep `ApplyLegacyIni`'s "No band3_config.ini" debug log for the missing case (test `std::filesystem::is_regular_file(LegacyIniPath())` for it). `ReadIniGameDataRoot` and `ApplyLegacyIni` use `ReadIni()` and drop their `const char* path` parameters (update their declarations and the two callers in `band3_app.h`); their log lines name the file with `rex::path_to_utf8(LegacyIniPath())`. `GameDataRoot` stores a `std::filesystem::path`; fix its two callers if they don't compile unchanged.

In `src/band3_app.h` replace `OnConfigurePaths`:

```cpp
  // paths are fixed before band3.toml loads, so band3_config.ini's are the ones
  // read here, relative to its folder; the command line wins over the ini
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto anchor = band3::IniAnchor();
    auto from_ini = [&](const char* cvar, const std::string& value, std::filesystem::path& out) {
      if (rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault || value.empty()) return false;
      out = band3::paths::Resolve(value, anchor);
      return true;
    };
    from_ini("game_data_root", band3::ReadIniGameDataRoot(), paths.game_data_root);
    const bool user_set = from_ini("user_data_root", band3::ReadIniString("user_data_root"),
                                   paths.user_data_root);
    const bool cache_set = from_ini("cache_root", band3::ReadIniString("cache_root"), paths.cache_root);
    // the SDK put the cache in the default user data folder; keep it with the new one
    if (user_set && !cache_set && rex::cvar::GetFlagSource("cache_root") == rex::cvar::Source::kDefault) {
      paths.cache_root = paths.user_data_root / "cache";
    }
    band3::SetGameDataRoot(paths.game_data_root);
  }
```

(Add `#include "paths.h"`.) At the top of `OnPostInitLogging`:

```cpp
    REXLOG_INFO("Folders: game data {}, user data {}, cache {} (ini: {})",
                rex::path_to_utf8(game_data_root()), rex::path_to_utf8(user_data_root()),
                rex::path_to_utf8(cache_root()), rex::path_to_utf8(band3::LegacyIniPath()));
```

`band3_config.ini` `[game]`, after `game_data_root`:

```ini
; Where band3 keeps saves, profiles and what the game writes (empty = a "band3"
; folder in your Documents folder), and its shader cache (empty = "cache" in the
; user data folder). A relative path is relative to this file's folder, so
; user_data_root = user_data keeps everything beside band3 (a portable install).
user_data_root =
cache_root =
```

README, in Settings after the `game:\` paragraph:

```markdown
`band3_config.ini` can also move band3's other folders: `user_data_root` (saves, profiles
and the `game` folder; `Documents\band3` by default) and `cache_root` (the shader cache;
`cache` in the user data folder by default). A relative path there is relative to the
ini's folder, so `user_data_root = user_data` keeps everything band3 writes beside it, for
a portable install. band3 looks for the ini in its working directory, then beside the
executable.
```

- [ ] **Step 6: Build, then check in the game.**

`cmd /c "out\b3env.cmd cmake --preset win-amd64-release"` (new source file), then build. A portable launch from a scratch folder with a non-ASCII name, not through `band3ctl` (it always passes `--user_data_root`, which would win):

```powershell
$t = "$PWD\out\portable_ü"; Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue; New-Item -ItemType Directory $t | Out-Null
"[game]`ngame_data_root = `"$PWD\assets`"`nuser_data_root = user_data`nfast_start = true" | Set-Content -Encoding utf8 "$t\band3_config.ini"
python -c "import subprocess,sys; si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=7; subprocess.Popen([sys.argv[1], '--test_port=21075', '--log_file='+sys.argv[2]+'\\run.log'], cwd=sys.argv[2], startupinfo=si, creationflags=0x8|0x200)" "$PWD\out\build\win-amd64-release\band3.exe" $t
```

Wait until `python tools/band3ctl.py --port 21075 state` answers, then `python tools/band3ctl.py --port 21075 quit`. Expected: `out\portable_ü\user_data\` holds the profile folder and `cache\`; `run.log` has the `Folders:` line naming those and the ini; nothing new under `Documents\band3`. (If `Set-Content -Encoding utf8` writes a BOM, check inih copes; if not, write the file without one.) Then confirm the default is unchanged: `python tools/band3ctl.py launch --fresh --user-data out/default_check`, `state`, `quit`; the log's `Folders:` line shows `assets` resolved in the worktree and `out/default_check`.

- [ ] **Step 7: Run unit tests, commit** (`src/paths.*`, `tests/paths_test.cpp`, `tests/CMakeLists.txt`, `src/config.*`, any changed `GameDataRoot` callers, `src/band3_app.h`, `band3_config.ini`, `README.md`). Delete `out/portable_ü` and `out/default_check`.

---

### Task 2: Reading package headers and scanning folders

**Files:**
- Create: `src/Content/package_scan.h`, `src/Content/package_scan.cpp`
- Create: `tests/package_scan_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces (namespace `band3::content`, no SDK includes):

```cpp
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
```

Header layout (big-endian), confirmed against the SDK's `stfs_xbox.h` and the sample packages: magic `0x000` (u32); licenses `0x22C`, 16 × {licensee_id u64, license_bits u32 at +8, license_flags u32 at +12}; content ID `0x32C` (20 bytes); content type `0x344` (u32); title ID `0x360` (u32); display name `0x411`, 128 UTF-16BE code units, NUL-terminated (English is the first language).

- [ ] **Step 1: Write the failing tests** — `tests/package_scan_test.cpp`:

```cpp
// Checks reading STFS package headers and finding packages in folders
// (src/Content/package_scan.cpp), with headers built here.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include "src/Content/package_scan.h"

namespace fs = std::filesystem;
using namespace band3::content;

namespace {

void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}

std::vector<uint8_t> MakeHeader(const char* magic, uint32_t type, uint32_t title, uint8_t id_byte,
                                const std::u16string& name) {
    std::vector<uint8_t> b(0x1000, 0);
    for (int i = 0; i < 4; i++) b[i] = uint8_t(magic[i]);
    for (int i = 0; i < 20; i++) b[0x32C + i] = uint8_t(id_byte + i);
    PutU32(b, 0x344, type);
    PutU32(b, 0x360, title);
    for (size_t i = 0; i < name.size(); i++) {
        b[0x411 + i * 2] = uint8_t(name[i] >> 8);
        b[0x411 + i * 2 + 1] = uint8_t(name[i]);
    }
    return b;
}

void Write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                std::streamsize(bytes.size()));
}

}

TEST_CASE("a custom song's header reads back") {
    auto b = MakeHeader("CON ", 1, kRb3TitleId, 0xA0, u"Accept - Balls to The Wall");
    // licence 0: all bits, flags set; licence 1: bits but no flags, ignored
    PutU32(b, 0x22C + 8, 0xFFFFFFFF); PutU32(b, 0x22C + 12, 1);
    PutU32(b, 0x23C + 8, 0x0000F000); PutU32(b, 0x23C + 12, 0);
    auto h = ParsePackageHeader(b);
    REQUIRE(h);
    CHECK(h->magic == 0x434F4E20);
    CHECK(h->content_type == 1);
    CHECK(h->title_id == kRb3TitleId);
    CHECK(h->license_mask == 0xFFFFFFFF);
    CHECK(h->content_id == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3");
    CHECK(h->display_name == u"Accept - Balls to The Wall");
}

TEST_CASE("LIVE and PIRS are packages; anything else isn't") {
    CHECK(ParsePackageHeader(MakeHeader("LIVE", 2, kRb3TitleId, 1, u"x")));
    CHECK(ParsePackageHeader(MakeHeader("PIRS", 2, kRb3TitleId, 1, u"x")));
    CHECK_FALSE(ParsePackageHeader(MakeHeader("RIFF", 2, kRb3TitleId, 1, u"x")));
    auto short_file = MakeHeader("CON ", 1, kRb3TitleId, 1, u"x");
    short_file.resize(kHeaderBytes - 1);
    CHECK_FALSE(ParsePackageHeader(short_file));
}

TEST_CASE("a display name filling all 128 characters has no terminator") {
    auto h = ParsePackageHeader(MakeHeader("CON ", 1, kRb3TitleId, 1, std::u16string(128, u'a')));
    REQUIRE(h);
    CHECK(h->display_name.size() == 128);
}

TEST_CASE("scanning keeps one of each RB3 package and skips the rest") {
    const fs::path root = fs::temp_directory_path() / "band3_scan_test";
    fs::remove_all(root);
    const auto song = MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"Song");
    Write(root / "a" / "Song_rb3con", song);
    Write(root / "a" / "sub" / "Song copy", song);              // same content ID: once
    Write(root / "a" / "Other", MakeHeader("CON ", 1, kRb3TitleId, 0x40, u"Other"));
    Write(root / "a" / "rb2_song", MakeHeader("CON ", 1, 0x45410869, 0x70, u"RB2"));  // other title
    Write(root / "a" / "notes.txt", std::vector<uint8_t>(10, 'x'));                    // junk
    std::vector<std::string> problems;
    auto found = ScanFolders({root / "a", root / "missing"}, kRb3TitleId, &problems);
    REQUIRE(found.size() == 2);
    CHECK(found[0].header.display_name != found[1].header.display_name);
    // the missing folder is reported by name; junk and the other title aren't problems
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("missing") != std::string::npos);
    fs::remove_all(root);
}
```

Add `package_scan_test.cpp` and `${BAND3_ROOT}/src/Content/package_scan.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run, see the build fail** (header missing).

- [ ] **Step 3: Implement** `src/Content/package_scan.h` with the **Interfaces** block (includes `<cstdint> <filesystem> <optional> <span> <string> <vector>`, a short header comment in the codebase's voice), and `src/Content/package_scan.cpp`:

```cpp
#include "package_scan.h"
#include <fstream>
#include <set>
#include <system_error>

namespace band3::content {

namespace {

uint32_t U32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

constexpr uint32_t kCon = 0x434F4E20, kLive = 0x4C495645, kPirs = 0x50495253;

std::string Utf8(const std::filesystem::path& path) {
    auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

}

std::optional<PackageHeader> ParsePackageHeader(std::span<const uint8_t> b) {
    if (b.size() < kHeaderBytes) return std::nullopt;
    PackageHeader h;
    h.magic = U32(b, 0);
    if (h.magic != kCon && h.magic != kLive && h.magic != kPirs) return std::nullopt;
    for (size_t i = 0; i < 16; i++) {
        const size_t at = 0x22C + i * 0x10;
        if (U32(b, at + 12)) h.license_mask |= U32(b, at + 8);
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < 20; i++) {
        h.content_id += kHex[b[0x32C + i] >> 4];
        h.content_id += kHex[b[0x32C + i] & 0xF];
    }
    h.content_type = U32(b, 0x344);
    h.title_id = U32(b, 0x360);
    for (size_t i = 0; i < 128; i++) {
        char16_t c = char16_t(b[0x411 + i * 2] << 8 | b[0x411 + i * 2 + 1]);
        if (!c) break;
        h.display_name += c;
    }
    return h;
}

std::vector<Package> ScanFolders(const std::vector<std::filesystem::path>& folders,
                                 uint32_t title_id, std::vector<std::string>* problems) {
    std::vector<Package> found;
    std::set<std::string> seen;
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) {
            if (problems) problems->push_back("no folder " + Utf8(folder));
            continue;
        }
        auto it = std::filesystem::recursive_directory_iterator(
            folder, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::vector<uint8_t> bytes(kHeaderBytes);
            std::ifstream file(it->path(), std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) continue;
            auto header = ParsePackageHeader(bytes);
            if (!header || header->title_id != title_id) continue;
            if (!seen.insert(header->content_id).second) continue;
            found.push_back({it->path(), std::move(*header)});
        }
        if (ec && problems) problems->push_back(Utf8(folder) + ": " + ec.message());
    }
    return found;
}

}
```

- [ ] **Step 4: Run tests: pass.** Then a one-off sanity check against the real share, not committed: a scratch doctest case or tiny program calling `ScanFolders({"\\\\BISHOP\\Downloads\\rb"}, kRb3TitleId, &problems)`. Expect 105 packages, all content type 1. Report the count and how long the scan took.

- [ ] **Step 5: Commit** (`src/Content/package_scan.*`, `tests/package_scan_test.cpp`, `tests/CMakeLists.txt`).

---

### Task 3: Pass-through overrides (de-risking spike)

Prove band3 can take over RB3's content calls, that the SDK's own XAM exports are callable from band3, and learn exactly which calls RB3 makes for songs. Nothing changes for the game yet.

**How RB3 reaches content (verified in `generated/`):**
- **Listing DLC and custom songs:** RB3's `XContentCreateCrossTitleEnumerator` (`0x8283EF70`, named in `band3_config.toml`) looks up XAM ordinal 633 (`XamContentAggregateCreateEnumerator`) at run time with `XexGetProcedureAddress` and calls it through a pointer. That lookup searches the SDK's own registry inside `rexruntime.dll`, so a band3 definition of `__imp__XamContentAggregateCreateEnumerator` is never reached. Override the recompiled function `XContentCreateCrossTitleEnumerator` instead (a `REX_FUNC` override of a named game function, like `rb3_main` in `src/Hooks/rb3dx.cpp`; the original is `__imp__XContentCreateCrossTitleEnumerator`). Its caller (around `sub_82520FC8`) loops: users 0–3 with content type 2, then user 255 with type 2, then user 255 with type 1. Arguments: r3 user_index, r4 device_id, r5 content_type, r6 (0), r7 (1), r8 buffer_size_ptr, r9 handle_out; it returns an X_RESULT in r3 and the handle at `handle_out`. The enumerator is the SDK's `XStaticEnumerator<XCONTENT_AGGREGATE_DATA>`.
- **The plain `XamContentCreateEnumerator`** (called directly by the generated code) is used only for saved games: `MemcardXbox__FindValidUnit` and `CacheMgrXbox__SearchAsync`, both type 1. band3 must never add packages there, or custom songs (also type 1) would appear as saves and song caches. It stays a pure pass-through; Task 3 only logs it.
- **Opening, closing, ownership:** the generated code calls `__imp__XamContentCreateEx`, `__imp__XamContentClose` and `__imp__XamContentGetCreator` directly by symbol (`DECLARE_REX_FUNC`, plain `extern "C"`; recompiled objects link straight into `band3.exe`), so a definition in band3 wins over the import library's lazy member. The SDK's originals are exported from the runtime DLL by exactly those names.
- `src/patches.cpp:300` forces `__imp__XamContentAggregateCreateEnumerator` into the link; leave it.

**Files:**
- Create: `src/Content/content_hooks.cpp`
- Modify: `CMakeLists.txt` (the runtime DLL's file name as a compile definition)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, in `content_hooks.cpp`: `extern "C"` overrides `XContentCreateCrossTitleEnumerator`, `__imp__XamContentCreateEnumerator`, `__imp__XamContentCreateEx`, `__imp__XamContentClose`, `__imp__XamContentGetCreator`, each `REX_FUNC`-shaped; and `namespace band3::content { bool ResolveSdkContentExports(); }` (declared in a new `src/Content/content_hooks.h`), called from `Band3App::OnPostSetup`, which looks the three SDK exports up once and returns false (after `REXLOG_ERROR` naming the missing one) if any is missing. Task 4 fills in band3's cases.

Argument registers for the XAM exports (big-endian guest memory; `REX_LOAD_U32(addr)`, `REX_RAW_ADDR(addr)` as used in `src/Game/DataNode.cpp`):
- `XamContentCreateEnumerator(user_index r3, device_id r4, content_type r5, content_flags r6, items_per_enumerate r7, buffer_size_ptr r8, handle_out r9)`
- `XamContentCreateEx(user_index r3, root_name r4, content_data r5, flags r6, disposition_ptr r7, license_mask_ptr r8, cache_size r9, content_size r10, overlapped at r1+0x54)`
- `XamContentClose(root_name r3, overlapped r4)`
- `XamContentGetCreator(user_index r3, content_data r4, is_creator_ptr r5, creator_xuid_ptr r6, overlapped r7)`
- `XCONTENT_DATA` is the SDK struct in `rex/system/xam/content_manager.h` (`content_type` at +0x04, `file_name()` reads `file_name_raw` at +0x108); read it through that struct, not hand-rolled offsets.

- [ ] **Step 1: The runtime DLL's name.** The SDK ships `rexruntime.dll`, `rexruntimed.dll` and `rexruntimerd.dll` for release, debug and relwithdebinfo builds. In `CMakeLists.txt`, under `if(WIN32)`: `target_compile_definitions(band3 PRIVATE BAND3_REXRUNTIME_DLL="$<TARGET_FILE_NAME:rex::runtime>")`. Check it expands as expected (the compile command in `out/build/win-amd64-release/build.ninja` shows `-DBAND3_REXRUNTIME_DLL=\"rexruntime.dll\"` after reconfigure).

- [ ] **Step 2: Write `content_hooks.h` / `content_hooks.cpp`:**

```cpp
#include "content_hooks.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xam/content_manager.h>
#include <rex/types.h>
#include "generated/band3_init.h"
#ifdef _WIN32
#include <windows.h>
#endif

// RB3 lists DLC and custom songs through XContentCreateCrossTitleEnumerator,
// and opens them, saves included, through these XAM exports. band3 takes them
// over so it can serve packages from content folders (live_content.cpp) and
// hands everything else to the SDK's own.

extern "C" void __imp__XContentCreateCrossTitleEnumerator(PPCContext& ctx, uint8_t* base);

namespace {

using Export = void(PPCContext&, uint8_t*);

Export* g_sdk_create_ex = nullptr;
Export* g_sdk_close = nullptr;
Export* g_sdk_get_creator = nullptr;
Export* g_sdk_create_enumerator = nullptr;

}

namespace band3::content {

bool ResolveSdkContentExports() {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto find = [&](const char* name, Export*& out) {
        out = runtime ? reinterpret_cast<Export*>(GetProcAddress(runtime, name)) : nullptr;
        if (!out) REXLOG_ERROR("content: {} has no {}", BAND3_REXRUNTIME_DLL, name);
        return out != nullptr;
    };
    bool ok = find("__imp__XamContentCreateEx", g_sdk_create_ex);
    ok &= find("__imp__XamContentClose", g_sdk_close);
    ok &= find("__imp__XamContentGetCreator", g_sdk_get_creator);
    ok &= find("__imp__XamContentCreateEnumerator", g_sdk_create_enumerator);
    return ok;
#else
    return false;
#endif
}

}

extern "C" REX_FUNC(XContentCreateCrossTitleEnumerator) {
    const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32;
    __imp__XContentCreateCrossTitleEnumerator(ctx, base);
    REXLOG_DEBUG("content: cross-title enumerator user {} device {} type {} -> {:#x}", user, device,
                 type, ctx.r3.u32);
}
```

and the same shape for the four XAM overrides: save every register you log **before** calling (`g_sdk_...(ctx, base)`), log after, at `REXLOG_DEBUG` (`CreateEnumerator`: user, device, type, flags; `CreateEx`: root name `reinterpret_cast<const char*>(REX_RAW_ADDR(ctx.r4.u32))`, `file_name()` and `content_type` of the `XCONTENT_DATA` at r5, `flags & 0xF`; `Close`: root name at r3; `GetCreator`: `file_name()` and type at r4). On non-Windows, these exports can't be redirected this way: wrap the four XAM overrides in `#ifdef _WIN32` so Linux links against the SDK's own as before (the cross-title override stays on both, as a pass-through).

In `band3_app.h` `OnPostSetup`, after `MountGameWrites`: `band3::content::ResolveSdkContentExports()`; on Windows a false return is fatal: `REXLOG_ERROR` and `std::abort()` — band3 can't open saves without them, so failing at startup beats failing on the first save.

For the spike run only, temporarily make the five log lines `REXLOG_INFO` (or run with `--log_level=debug`).

- [ ] **Step 3: Build** (`cmd /c "out\b3env.cmd cmake --preset win-amd64-release"`, then build). If the link fails with duplicate symbols, stop and report the exact error; don't edit generated code to work around it.

- [ ] **Step 4: Run the game** with fresh user data through `boot.b3t` with `--log_level=debug`, quit, relaunch with the same user data (no `--fresh`), wait for `splash_screen`, quit. Expected: the game behaves as before (profile created, then found again on the second launch); the log shows the calls. Record in your report, verbatim: every cross-title enumerator call (user, device, type, result), every `XamContentCreateEnumerator` call, the root names and content types RB3 passes to `CreateEx`, and any `GetCreator` calls. Task 4's listing rule depends on this.

- [ ] **Step 5: Commit** (`src/Content/content_hooks.*`, `CMakeLists.txt`, `src/band3_app.h`) with the logging at debug level; the message says what the spike showed.

---

### Task 4: Serving packages from content folders

**Files:**
- Create: `src/Content/live_content.h`, `src/Content/live_content.cpp`
- Modify: `src/Content/content_hooks.cpp` (band3's cases)
- Modify: `src/settings.cpp`, `src/settings.h` (`content_folders`)
- Modify: `src/config.cpp` (`kIniSettings` row `{"game", "content_folders", "content_folders"}`)
- Modify: `src/band3_app.h` (start the scan in `OnPostSetup`)
- Modify: `band3_config.ini`, `README.md`

**Interfaces:**
- Consumes: `band3::paths::Resolve`, `band3::paths::SplitList` (Task 1); `band3::IniAnchor()` (Task 1); `band3::content::ScanFolders`, `Package`, `PackageHeader`, `kRb3TitleId` (Task 2); the overrides and `ResolveSdkContentExports` (Task 3).
- Produces (`src/Content/live_content.h`, namespace `band3::content`):

```cpp
// scans the content_folders setting's folders on a worker thread; call once,
// before the game starts
void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs);

// band3's packages, once the scan is done (waits for it, up to `timeout`;
// empty if it isn't done by then)
const std::vector<Package>& LivePackages(std::chrono::milliseconds timeout);

// the package named file_name (its content ID), if it is one of band3's
const Package* FindLivePackage(std::string_view file_name);

// mounts package as root_name: (as XamContentCreateEx does); false if it won't mount
bool MountLivePackage(const Package& package, std::string_view root_name);

// unmounts root_name: if band3 mounted it; false if it's not band3's
bool UnmountLiveRoot(std::string_view root_name);
```

Setting (in `src/settings.cpp`, with the other `Band3/Game` ones; declare it in `settings.h`):

```cpp
REXCVAR_DEFINE_STRING(content_folders, "songs", "Band3/Game",
    "Folders RB3 reads DLC and custom songs from, without installing them, separated by '|'. "
    "Subfolders count too. A relative folder is relative to band3_config.ini's folder")
    .lifecycle(Lifecycle::kRequiresRestart);
```

- [ ] **Step 1: `live_content.cpp`.**
  - `StartLiveContent` starts a `std::thread` (detached, or joined at shutdown via a static object) that resolves each `SplitList(REXCVAR_GET(content_folders))` entry with `Resolve(entry, IniAnchor())`, runs `ScanFolders(..., kRb3TitleId, &problems)`, logs each problem (`REXLOG_WARN`, except `REXLOG_DEBUG` when the folder is the default `songs` and the setting is unchanged), then `REXLOG_INFO("content: {} packages from {} in {} ms", ...)`, stores the result and signals completion (`std::promise`/`std::shared_future`, or a mutex + condition variable). An unreachable network share can take tens of seconds; the scan must not hold up startup.
  - `LivePackages(timeout)` waits on completion up to `timeout` and returns the stored vector (a static empty one if not done). `FindLivePackage` uses `LivePackages(0ms)` and compares `file_name` with `header.content_id`, case-insensitively.
  - Mounting:

```cpp
bool MountLivePackage(const Package& package, std::string_view root_name) {
    std::lock_guard lock(g_mutex);
    UnmountLocked(root_name);  // band3's own earlier mount of this root, if any
    // the trailing separator keeps \Device\Band3Content\1\ from matching ...\10\ (the VFS
    // takes the first device whose mount path is a prefix)
    const std::string mount = fmt::format("\\Device\\Band3Content\\{}\\", ++g_next_mount);
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>(mount, package.path);
    if (!device->Initialize()) {
        REXLOG_WARN("content: can't read {}", rex::path_to_utf8(package.path));
        return false;
    }
    const std::string link = std::string(root_name) + ":";
    g_vfs->UnregisterSymbolicLink(link);
    g_vfs->RegisterDevice(std::move(device));
    g_vfs->RegisterSymbolicLink(link, mount);
    g_mounts[Lower(root_name)] = mount;
    return true;
}
```

  `UnmountLiveRoot` (and `UnmountLocked`) look the root up in `g_mounts` (lower-cased), unregister the symlink and the device (`UnregisterDevice(mount)`), erase it, return true; false if absent. Before mounting, if the SDK has a package open on that root, close it first: `REX_KERNEL_STATE()->content_manager()->CloseContent(root_name)` (the SDK does the same for CREATE_ALWAYS).

- [ ] **Step 2: band3's cases in `content_hooks.cpp`.**
  - **Cross-title enumerator:** save user, device, type and the `handle_out` address (r9) before calling the original. If it returned 0, `device` is 0 or `DummyDeviceId::HDD` (1), **`user == 0xFF`**, append one item per package in `LivePackages(std::chrono::seconds(15))` whose `header.content_type == type`: look the enumerator up with `REX_KERNEL_OBJECTS()->LookupObject<rex::system::XStaticEnumerator<XCONTENT_AGGREGATE_DATA>>(handle)` (`rex/system/xenumerator.h`), then `auto* item = e->AppendItem();` and set `device_id = 1`, `content_type`, `title_id = kRb3TitleId`, `xuid = 0`, `set_display_name(header.display_name)`, `set_file_name(header.content_id)`. If Task 3's log showed a different user/type pattern than users 0–3 type 2, then 255 type 2, then 255 type 1, pick the rule that lists each package exactly once with its own type, and say so in the commit message. Log `REXLOG_INFO("content: listed {} packages of type {}", n, type)` when n > 0.
  - **`XamContentCreateEnumerator`:** unchanged pass-through. Never add packages here.
  - **CreateEx:** if `FindLivePackage(data.file_name())` with `header.content_type == data.content_type`: handle it with a typed function through `rex::ppc::HostToGuestFunction<Handler>(ctx, base)` using the SDK's signature (copy the parameter types from the SDK's `XamContentCreateEx_entry`: `u32 user_index, mapped_string root_name, mapped_void content_data_ptr, u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr, u32 cache_size, u64 content_size, mapped_void overlapped_ptr`). `flags & 0xF` of 3 (OPEN_EXISTING) or 4 (OPEN_ALWAYS): mount, disposition 2 (open), `license_mask = header.license_mask`, result `X_ERROR_SUCCESS`; mount failure: `X_ERROR_FILE_CORRUPT`; any other disposition: `X_ERROR_ACCESS_DENIED` (band3's packages are read-only). Write `*disposition_ptr` and `*license_mask_ptr` when set. Complete like the SDK: with `overlapped_ptr`, `REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result, X_HRESULT_FROM_WIN32(result), disposition)` and return `X_ERROR_IO_PENDING`; without, return the result. Not band3's: if the root is band3-mounted, `UnmountLiveRoot(root)` first (the SDK's symlink insert won't replace band3's), then the SDK's.
  - **Close:** `UnmountLiveRoot(root)`; if true complete with success (overlapped as above), else the SDK's.
  - **GetCreator:** band3's package → `*is_creator_ptr = 0`, `*creator_xuid_ptr = 0` if set, success (overlapped as above); else the SDK's.

- [ ] **Step 3: Wire up.** `band3_app.h` `OnPostSetup`: `band3::content::StartLiveContent(runtime()->file_system());` right after `ResolveSdkContentExports()`. `config.cpp` ini row. `band3_config.ini` `[game]`:

```ini
; Folders Rock Band 3 reads DLC and custom songs (CON, LIVE) from, as they are:
; nothing is installed or unpacked. Separate folders with | (a ; starts a comment
; here); subfolders count. A relative folder is relative to this file's folder.
; Changes apply at the next launch.
content_folders = songs
```

  README: a short `### DLC and custom songs` section under Settings saying the above, that a network folder works but a local one is safer for audio, and that a song's `song_id` written as text is corrected (Task 5 adds that sentence).

- [ ] **Step 4: Build and check in the game.** Copy into `out/test_songs/`: `Get Got x1 v2`, `KMFDMMega_rb3con`, `letsgetitstartedfv4b_rb3con`, `Death Cab For Cutie - Transatlanticism (Tribue to Six Feet Under OST)_v1.2_rb3con`. Launch fresh with `-- "--content_folders=<absolute out/test_songs>" --http_enabled=true --log_file=...` (quote arguments holding `|`; PowerShell treats a bare `|` as a pipe), run `boot.b3t`. Expected:
  - log: `content: 4 packages from ...` (letsgetitstarted holds more than one song but is one package), and `listed 4 packages of type 1` once per boot;
  - `GET http://127.0.0.1:21070/list_songs` lists the songs in those packages (shortnames from their `songs.dta`); record which appear;
  - start one on autoplay through Quickplay (see `tests/game/render_song.b3t` for the menu path; `/jump?shortname=` selects it in the open Music Library), `wait in_game`, `wait score>=1`, and let it play to the end: no audio glitches (Review Focus 3); also scroll the Music Library over the four so their previews play;
  - quit, relaunch with the same user data: the songs are still listed and the profile loads (Review Focus 1);
  - relaunch with `"--content_folders=C:/nope|\\\\NOHOST\\share|<test_songs>"`: a warning names each missing folder, the 4 packages still load, and report how long the menus took to appear compared with the run before (Review Focus 2).
  Then once with `"--content_folders=\\\\BISHOP\\Downloads\\rb"`: report the package count (expect 105), scan time, and whether a song plays through without glitches.

- [ ] **Step 5: Commit** (all files above).

---

### Task 5: Correcting text song IDs

**Files:**
- Create: `src/Game/song_id.h`, `src/Game/song_id.cpp`
- Create: `tests/song_id_test.cpp`
- Modify: `src/Game/DataNode.cpp` (the `DataNode__Int` override)
- Create: `src/Hooks/song_id.cpp` (the `GetSongID` override)
- Modify: `tests/CMakeLists.txt` (add `song_id_test.cpp` and `${BAND3_ROOT}/src/Game/song_id.cpp`), `README.md` (one sentence in the DLC section)

**Interfaces:**
- Produces (`src/Game/song_id.h`, namespace `band3`, no SDK includes):
  - `uint32_t Crc32(std::string_view text);` standard CRC-32.
  - `int32_t CorrectedSongId(std::string_view text);` `int32_t(Crc32(text) % 9999999 + 2130000000)`.
  - `void LogSongIdCorrection(std::string_view text, int32_t id);` logs once per distinct text (mutex-guarded set). This one may live in `src/Hooks/song_id.cpp` instead if it needs `<rex/logging.h>`; keep `song_id.h`'s unit-tested part SDK-free.
- Consumes: `literal_str`'s rule in `src/Game/DataNode.cpp:33-37` for a node's text: a `kDataSymbol` node's value is the text's guest address; a `kDataString` node's value is the address of a block whose first u32 is the text's address.

- [ ] **Step 1: Failing tests** — `tests/song_id_test.cpp`:

```cpp
// Checks the song ID a custom song with a text song_id gets
// (src/Game/song_id.cpp): RB3Enhanced's, so the two agree.

#include <doctest/doctest.h>
#include "src/Game/song_id.h"

TEST_CASE("CRC-32 is the standard one") {
    CHECK(band3::Crc32("123456789") == 0xCBF43926u);
    CHECK(band3::Crc32("") == 0u);
}

TEST_CASE("text song IDs become RB3Enhanced's numbers") {
    CHECK(band3::CorrectedSongId("KMFDMMega") == 2133239510);
    CHECK(band3::CorrectedSongId("dust_pc") == 2133960588);
    CHECK(band3::CorrectedSongId("mardigras_pc") == 2138512288);
    CHECK(band3::CorrectedSongId("123456789") == 2131780604);
}
```

(Values computed with `zlib.crc32`, which matches RB3Enhanced's `crc32.c` table code.)

- [ ] **Step 2: Run, see it fail. Step 3: implement** `song_id.h/.cpp` (a 256-entry table built once from polynomial `0xEDB88320`; `crc = ~0`, final `~crc`). **Step 4: tests pass.**

- [ ] **Step 5: The two read sites.** No codegen: both go through functions band3 already overrides or can override by name.
  1. **`SongMetadata`'s constructor** reads `song_id` with `addi r3,r11,8` / `bl 0x8274B0F8` (`DataNode__Int`) / `stw r3,48(r27)`; the generated call sets `ctx.lr = 0x827AA7D8` just before calling. `DataNode__Int` is already overridden in `src/Game/DataNode.cpp:49`. There: if `ctx.lr == 0x827AA7D8` and the evaluated node is `kDataSymbol` or `kDataString`, return `CorrectedSongId(text)` (text by `literal_str`'s rule) and log the correction; otherwise behave exactly as now. Every other caller of `DataNode__Int` is untouched.
  2. **`GetSongID`** (`0x827A87F0`, `DataArray* song` r3, `DataArray* missing_data` r4) looks `song_id` up with `DataArray::FindData` (`0x8274C7F0`): in `song` first, then in `missing_data` only if that gave 0 and `missing_data` is non-null; returns the int. Override it in `src/Hooks/song_id.cpp` with `extern "C" REX_FUNC(GetSongID)`: **save r3 and r4**, call `__imp__GetSongID(ctx, base)` (pattern: `src/Hooks/rb3dx.cpp`), then find the node in host code in the same order: walk the array (`DataArray.mNodes` → nodes of 8 bytes: value, type; `mSize` entries), find the child array (type `kDataArray`, 16) whose node 0 is the symbol `song_id` (compare with `band3::Symbol(ctx, base, "song_id").value(base)`, built once), take its node 1. If it's `kDataSymbol` or `kDataString`, set r3 to `CorrectedSongId(text)` and log; otherwise leave the original's result. Check the `missing_data` array only when the original would have (nothing found in `song`).

- [ ] **Step 6: Build and check in the game.** With `KMFDMMega_rb3con` (text ID `KMFDMMega`) and `Get Got x1 v2` (numeric `2001610165`) in a content folder and HTTP on: before this task's change (Task 4's build), record `GET /song_2133239510` (expect 404) and `/song_2001610165`. After: `/song_2133239510` returns KMFDM's song, `/song_2001610165` still returns Get Got's, and the log has the correction line for `KMFDMMega` once. Relaunch with the same user data: same results.

- [ ] **Step 7: Commit** (`src/Game/song_id.*`, `tests/song_id_test.cpp`, `tests/CMakeLists.txt`, `src/Game/DataNode.cpp`, `src/Hooks/song_id.cpp`, `README.md`).
