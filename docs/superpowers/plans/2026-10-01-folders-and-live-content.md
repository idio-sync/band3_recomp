# Folders, Live Content and Song IDs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make band3's folders configurable from the ini (portable installs), let RB3 read DLC and custom songs straight from folders without unpacking, and correct custom songs' text `song_id`s as RB3Enhanced does.

**Architecture:** Pure, SDK-free logic (path resolution, STFS header parsing, folder scanning, CRC song IDs) lives in small files the unit tests build. The SDK-facing parts are thin: `Band3App::OnConfigurePaths` for folders; overrides of five XAM exports that handle band3's packages and call the SDK's own (looked up in `rexruntime.dll`) for everything else; a `GetSongID` override and two midasm hooks for song IDs.

**Tech Stack:** C++23, clang-cl via Ninja, ReXGlue SDK 0.10 (prebuilt in `.rexglue-sdk`), doctest unit tests (`tests/`), the game test harness (`tools/band3ctl.py`, `.b3t` scripts).

**Spec:** `docs/superpowers/specs/2026-10-01-folders-and-live-content-design.md`

## Global Constraints

- Code style: 4-space indent, lowercase terse comments in the codebase's voice, `namespace band3::...`, match neighbouring files. No new third-party dependencies.
- Unit-tested code must not include any `<rex/...>` header (the tests build without the SDK); list every new test file and every `src/` file it needs in `tests/CMakeLists.txt`.
- Command line wins over `band3.toml`, which wins over `band3_config.ini` (existing rule; see `src/config.cpp` `ApplyLegacyIni`).
- A relative path in the ini, or in `content_folders`, is relative to the folder `band3_config.ini` was found in (its "anchor"); with no ini found, the working directory.
- RB3's title ID is `0x45410914`.
- Song ID correction: `crc32(text) % 9999999 + 2130000000`, standard CRC-32 (zlib's), the same as RB3Enhanced.
- Never write to `assets/` or to a content folder.
- Test launches start minimized and never take focus: use `tools/band3ctl.py launch` (it does), or `SW_SHOWMINNOACTIVE` if starting `band3.exe` another way.
- Commit after each task with the repo's message style (imperative summary, explanatory body) ending in `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Git has no identity configured: commit with `git -c user.name=idio-sync -c user.email=jedivoodoo@gmail.com commit ...`.

### Building and testing (from the worktree root, PowerShell)

The shell has neither clang nor Ninja on `PATH`; every build goes through this:

```powershell
$vc = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$ninja = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
cmd /c "`"$vc`" >nul 2>nul && set `"PATH=C:\Program Files\LLVM\bin;$ninja;%PATH%`" && cmake --build out/build/win-amd64-release 2>&1"
```

Unit tests: same wrapper with `cmake --build out/tests && ctest --test-dir out/tests --output-on-failure`. A new `src/*.cpp` file needs `cmake --preset win-amd64-release` first (the source list is a glob). Changing `band3_config.toml` needs codegen first: `.\.rexglue-sdk\bin\rexglue.exe codegen band3_manifest.toml` (~2 minutes).

Game runs: `python tools/band3ctl.py launch --fresh --user-data out/<name> -- --log_file=<absolute path>.log` then `python tools/band3ctl.py run tests/game/boot.b3t` to reach the main menu on Rock Band 3 Deluxe (`assets/default.xex` is Deluxe). `python tools/band3ctl.py quit` when done. The HTTP server (`-- --http_enabled=true`, port 21070) answers `GET /list_songs` (every song's shortname, title, artist) and `GET /song_<id>` (one song by ID): use it to prove songs are in the library.

Sample packages (custom songs, all `CON`, content type 1, title `45410914`) are on `\\BISHOP\Downloads\rb`. Never modify or delete them; copy the ones a task names into `out/test_songs/`.

## Review Focus

1. **Saves after the XAM overrides:** the profile and Deluxe's save must still create and load (they go through the same `XamContentCreateEx`). Tasks 3 and 4 relaunch with the same user data and check the profile loads.
2. **A content folder that's missing or unreachable** (a NAS that's off): startup continues, a warning names the folder, no crash. Task 2 tests a missing folder; Task 4 sets a nonexistent one in the game.
3. **The same package in two folders, or junk files beside packages:** listed once; junk ignored. Task 2's scan test.
4. **`user_data_root` set in the ini without `cache_root`:** the cache follows the user data root rather than staying in `Documents\band3\cache`. Task 1's portable launch checks `user_data\cache` exists.
5. **Odd file names:** 81-character names, spaces, `+`, `(`, `'`: the package's name to the game is its content ID, not its file name. Task 4 uses the 81-character Death Cab file.

---

### Task 1: Folders from the ini (portability)

**Files:**
- Create: `src/paths.h`, `src/paths.cpp`
- Create: `tests/paths_test.cpp`
- Modify: `tests/CMakeLists.txt` (add `paths_test.cpp` and `${BAND3_ROOT}/src/paths.cpp`)
- Modify: `src/config.h`, `src/config.cpp` (find the ini; read three path keys)
- Modify: `src/band3_app.h` (`OnConfigurePaths`, `OnPostInitLogging`'s `ApplyLegacyIni` call)
- Modify: `band3_config.ini` (`[game]` documents the new keys)
- Modify: `README.md` (Settings section)

**Interfaces:**
- Produces (in `src/paths.h`, namespace `band3::paths`, no SDK includes):
  - `std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);` empty → empty path; absolute → itself; relative → `(anchor / value).lexically_normal()`.
  - `std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs, std::string_view name);` first `dir / name` that is a regular file, else empty.
  - `std::vector<std::string> SplitList(std::string_view value);` splits on `;`, trims spaces and tabs, drops empty parts.
- Produces (in `src/config.h`, namespace `band3`):
  - `const std::filesystem::path& LegacyIniPath();` the ini found (working directory, then beside the exe), or `kLegacyIniPath` relative if neither has one. Found once, then cached.
  - `std::filesystem::path IniAnchor();` the folder `LegacyIniPath()` is in, absolute; the working directory if no ini was found.
  - `std::string ReadIniString(const char* key);` `[game] key` from the ini, unquoted (empty if missing). `ReadIniGameDataRoot` keeps its `[paths]` fallback and `"assets"` default and is reimplemented on top of it.

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
    const fs::path anchor = fs::path("C:/games/band3");
    CHECK(Resolve("user_data", anchor) == fs::path("C:/games/band3/user_data").lexically_normal());
    CHECK(Resolve("../shared/songs", anchor) == fs::path("C:/games/shared/songs").lexically_normal());
}

TEST_CASE("an absolute path is kept and an empty one stays empty") {
    const fs::path anchor = fs::path("C:/games/band3");
    CHECK(Resolve("D:/rb/songs", anchor) == fs::path("D:/rb/songs"));
    CHECK(Resolve("//BISHOP/Downloads/rb", anchor) == fs::path("//BISHOP/Downloads/rb"));
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

TEST_CASE("a folder list splits on semicolons and drops blanks") {
    CHECK(SplitList("").empty());
    CHECK(SplitList(" ; ;").empty());
    CHECK(SplitList("songs") == std::vector<std::string>{"songs"});
    CHECK(SplitList(" songs ; \\\\BISHOP\\Downloads\\rb ;D:/dlc") ==
          std::vector<std::string>{"songs", "\\\\BISHOP\\Downloads\\rb", "D:/dlc"});
    // spaces inside a folder's name are kept
    CHECK(SplitList("My Songs;Other") == std::vector<std::string>{"My Songs", "Other"});
}
```

Add both files to `tests/CMakeLists.txt`'s `add_executable(band3_tests ...)` list (keep its ordering style: test files with the other tests, `${BAND3_ROOT}/src/paths.cpp` with the other sources).

- [ ] **Step 2: Run the tests to see them fail** — configure once if `out/tests` is missing (`cmake -S tests -B out/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++`), then build: compile error, `src/paths.h` not found.

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

// value resolved against anchor; an empty value stays empty
std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);

// the first dir / name that is a file, or empty
std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name);

// "a; b ;;c" -> {"a", "b", "c"}
std::vector<std::string> SplitList(std::string_view value);

}
```

```cpp
#include "paths.h"
#include <system_error>

namespace band3::paths {

std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor) {
    if (value.empty()) return {};
    std::filesystem::path path(value);
    if (path.is_absolute()) return path;
    return (anchor / path).lexically_normal();
}

std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name) {
    std::error_code ec;
    for (const auto& dir : dirs) {
        auto candidate = dir / name;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<std::string> SplitList(std::string_view value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(';', start);
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

Note on `Resolve` and UNC paths: `std::filesystem::path("//BISHOP/x").is_absolute()` is true on Windows; the test pins it.

- [ ] **Step 4: Run the tests to see them pass.**

- [ ] **Step 5: Wire it into config and the app.**

In `src/config.h` add the three declarations from **Interfaces** (include `<filesystem>`). In `src/config.cpp`:

```cpp
#include <rex/filesystem.h>   // GetExecutableFolder
#include "paths.h"

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
    INIReader reader(LegacyIniPath().string());
    return Unquote(reader.Get("game", key, ""));
}
```

`ReadIniGameDataRoot` and `ApplyLegacyIni` take the path from `LegacyIniPath()` (drop their `const char* path` default-argument use at call sites; keep the parameter if other callers pass one, else remove it). Their log messages keep naming the file.

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
    band3::SetGameDataRoot(paths.game_data_root.string());
  }
```

(Add `#include "paths.h"`.) Then log the folders once logging is up: at the top of `OnPostInitLogging`, `REXLOG_INFO("Folders: game data {}, user data {} (ini: {})", game_data_root().string(), user_data_root().string(), band3::LegacyIniPath().string());` (`ReXApp` has both accessors).

`band3_config.ini` `[game]`, after `game_data_root`:

```ini
; Where band3 keeps saves, profiles and what the game writes (empty = Documents\band3)
; and its shader cache (empty = "cache" in the user data folder). A relative path is
; relative to this file's folder, so user_data_root = user_data keeps everything
; beside band3 (a portable install).
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

Build (re-run `cmake --preset win-amd64-release` first: new source file). Then a portable launch from a scratch folder, not through `band3ctl` (it always passes `--user_data_root`, which would win):

```powershell
$t = "$PWD\out\portable_check"; Remove-Item -Recurse -Force $t -ErrorAction SilentlyContinue; New-Item -ItemType Directory $t | Out-Null
"[game]`ngame_data_root = `"$PWD\assets`"`nuser_data_root = user_data`nfast_start = true" | Set-Content -Encoding utf8 "$t\band3_config.ini"
python -c "import subprocess,sys; si=subprocess.STARTUPINFO(); si.dwFlags|=subprocess.STARTF_USESHOWWINDOW; si.wShowWindow=7; subprocess.Popen([sys.argv[1], '--test_port=21075', '--log_file='+sys.argv[2]+'\\run.log'], cwd=sys.argv[2], startupinfo=si, creationflags=0x8|0x200)" "$PWD\out\build\win-amd64-release\band3.exe" $t
```

Wait until `python tools/band3ctl.py --port 21075 state` answers, then `python tools/band3ctl.py --port 21075 quit`. Expected: `out\portable_check\user_data\` exists and holds the profile folder and `cache\`; `run.log` has the `Folders:` line naming those; nothing new under `Documents\band3`. Then confirm the default is unchanged: `python tools/band3ctl.py launch --fresh --user-data out/default_check`, `state`, `quit`, and the log/`out/default_check` look as before.

- [ ] **Step 7: Run unit tests, commit** (`src/paths.*`, `tests/paths_test.cpp`, `tests/CMakeLists.txt`, `src/config.*`, `src/band3_app.h`, `band3_config.ini`, `README.md`). Delete `out/portable_check` and `out/default_check` (they're ignored, but don't leave them).

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
// ID (the first found); `problems` gets one line per folder or file skipped
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
    // the missing folder is reported; junk and the other title aren't errors worth a line each
    bool missing_reported = false;
    for (auto& p : problems) missing_reported |= p.find("missing") != std::string::npos;
    CHECK(missing_reported);
    fs::remove_all(root);
}
```

Add `package_scan_test.cpp` and `${BAND3_ROOT}/src/Content/package_scan.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run, see the build fail** (header missing).

- [ ] **Step 3: Implement** `src/Content/package_scan.h` with the **Interfaces** block (includes `<cstdint> <filesystem> <optional> <span> <string> <vector>`, a short header comment in the codebase's voice), and `src/Content/package_scan.cpp`:

```cpp
#include "package_scan.h"
#include <cstdio>
#include <fstream>
#include <set>
#include <system_error>

namespace band3::content {

namespace {

uint32_t U32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

constexpr uint32_t kCon = 0x434F4E20, kLive = 0x4C495645, kPirs = 0x50495253;

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
            if (problems) problems->push_back("no folder " + folder.string());
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
        if (ec && problems) problems->push_back(folder.string() + ": " + ec.message());
    }
    return found;
}

}
```

- [ ] **Step 4: Run tests: pass.** Also run the scan against the real share once, as a sanity check, with a throwaway doctest case you don't commit (or a scratch program): expect 105 packages from `\\BISHOP\Downloads\rb`, all content type 1. Report the count.

- [ ] **Step 5: Commit** (`src/Content/package_scan.*`, `tests/package_scan_test.cpp`, `tests/CMakeLists.txt`).

---

### Task 3: Pass-through XAM overrides (de-risking spike)

Prove band3 can supply its own XAM content exports, that the SDK's own are callable, and learn exactly which calls RB3 makes for songs. Nothing changes for the game yet.

**Files:**
- Create: `src/Content/xam_content_hooks.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: in `xam_content_hooks.cpp`, `extern "C"` definitions of `__imp__XamContentCreateEnumerator`, `__imp__XamContentAggregateCreateEnumerator`, `__imp__XamContentCreateEx`, `__imp__XamContentClose`, `__imp__XamContentGetCreator`, each `REX_FUNC`-shaped `(PPCContext& ctx, uint8_t* base)`; and a file-local `template <class F> F* Sdk(const char* name)` that returns the SDK's export of that name from `rexruntime.dll`. Task 4 fills in the band3 cases.

Facts to rely on:
- The generated code calls these by symbol (e.g. `generated/band3_recomp.31.cpp` calls `__imp__XamContentCreateEx(ctx, base)`) and registers them for indirect calls (`generated/band3_register.cpp`). A definition in band3's own object file wins over the import library's, the way `REX_FUNC` overrides of game functions already do (see `src/Hooks/rb3dx.cpp`).
- `rexruntime.dll` exports them by exactly these names (`llvm-readobj --coff-exports .rexglue-sdk/bin/rexruntime.dll`).
- `src/patches.cpp` already forces `__imp__XamContentAggregateCreateEnumerator` into the link ("so dlc can load through its roundabout way"); keep that line working (if defining the symbol here makes it redundant, leave it — don't widen this task).
- Argument registers (big-endian guest memory at `base + address`; `REX_LOAD_U32` reads it):
  - `XamContentCreateEnumerator(user_index r3, device_id r4, content_type r5, content_flags r6, items_per_enumerate r7, buffer_size_ptr r8, handle_out r9)`
  - `XamContentAggregateCreateEnumerator(xuid r3, device_id r4, content_type r5, unk r6, handle_out r7)`
  - `XamContentCreateEx(user_index r3, root_name r4, content_data r5, flags r6, disposition_ptr r7, license_mask_ptr r8, cache_size r9, content_size r10, overlapped on the stack)`
  - `XamContentClose(root_name r3, overlapped r4)`
  - `XamContentGetCreator(user_index r3, content_data r4, is_creator_ptr r5, creator_xuid_ptr r6, overlapped r7)`
  - `XCONTENT_DATA` (`rex/system/xam/content_manager.h`): `device_id` u32 `+0x00`, `content_type` u32 `+0x04`, display name `+0x08`, `file_name_raw[42]` `+0xFC`; use the SDK struct, don't hand-roll offsets.

- [ ] **Step 1: Write the pass-through file:**

```cpp
#include <windows.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xam/content_manager.h>
#include <rex/types.h>
#include "generated/band3_init.h"

// RB3 reaches DLC and saves through these XAM exports. band3 supplies them so it
// can serve packages from content folders (src/Content/live_content.cpp) and
// hands every other call to the SDK's own, which rexruntime.dll exports by name.

namespace {

using Export = void(PPCContext&, uint8_t*);

Export* Sdk(const char* name) {
    static HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
    auto* fn = reinterpret_cast<Export*>(GetProcAddress(runtime, name));
    if (!fn) REXLOG_ERROR("xam: rexruntime.dll has no {}", name);
    return fn;
}

}

extern "C" REX_FUNC(__imp__XamContentCreateEnumerator) {
    static Export* sdk = Sdk("__imp__XamContentCreateEnumerator");
    const uint32_t type = ctx.r5.u32, device = ctx.r4.u32;
    sdk(ctx, base);
    REXLOG_INFO("xam: CreateEnumerator type {} device {} -> {:#x}", type, device, ctx.r3.u32);
}
```

…and the same shape for the other four, each logging its interesting arguments (root name string for Create/Close — `reinterpret_cast<const char*>(base + ctx.r4.u32)` for CreateEx, `r3` for Close; content type and `file_name()` from the `XCONTENT_DATA` at `r5`/`r4`; flags) **before** calling the SDK (registers change after). Save every register you log before the call.

- [ ] **Step 2: Build** (`cmake --preset win-amd64-release` first: new file). If the link fails with duplicate symbols, stop and report the exact error — the plan's approach depends on this; don't work around it by editing generated code.

- [ ] **Step 3: Run the game** with fresh user data through `boot.b3t`, quit, relaunch with the same user data (no `--fresh`), wait for `splash_screen`, quit. Expected: the game behaves as before (profile created, then found again on the second launch); the log shows the calls. Record in your report, verbatim: which enumerator(s) RB3 calls, with which content types and device IDs, how many times, and the root names it uses for CreateEx. That list decides Task 4's details.

- [ ] **Step 4: Commit** the file with the logging at `REXLOG_DEBUG` (not info) so normal logs stay quiet; message says what the spike showed.

---

### Task 4: Serving packages from content folders

**Files:**
- Create: `src/Content/live_content.h`, `src/Content/live_content.cpp`
- Modify: `src/Content/xam_content_hooks.cpp` (band3's cases)
- Modify: `src/settings.cpp`, `src/settings.h` (`content_folders`)
- Modify: `src/config.cpp` (`kIniSettings` row `{"game", "content_folders", "content_folders"}`)
- Modify: `src/band3_app.h` (start it in `OnPostSetup`, after `MountGameWrites`)
- Modify: `band3_config.ini`, `README.md`

**Interfaces:**
- Consumes: `band3::paths::Resolve`, `band3::paths::SplitList` (Task 1); `band3::IniAnchor()` (Task 1); `band3::content::ScanFolders`, `Package`, `PackageHeader`, `kRb3TitleId` (Task 2); the five overrides and `Sdk()` (Task 3).
- Produces (`src/Content/live_content.h`, namespace `band3::content`):

```cpp
// scans the content_folders setting's folders; call once, before the game starts
void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs);

// the package named file_name (its content ID), if it is one of band3's
const Package* FindLivePackage(std::string_view file_name);

// band3's packages of content_type, for an enumerator
std::vector<const Package*> LivePackagesOfType(uint32_t content_type);

// mounts package as root_name: (as XamContentCreateEx does); false if it won't mount
bool MountLivePackage(const Package& package, std::string_view root_name);

// unmounts root_name: if band3 mounted it; false if it's not band3's
bool UnmountLiveRoot(std::string_view root_name);
```

Setting (in `src/settings.cpp`, with the other `Band3/Game` ones):

```cpp
REXCVAR_DEFINE_STRING(content_folders, "songs", "Band3/Game",
    "Folders RB3 reads DLC and custom songs from, without installing them, separated by ';'. "
    "Subfolders count too. A relative folder is relative to band3_config.ini's folder")
    .lifecycle(Lifecycle::kRequiresRestart);
```

and `REXCVAR_DECLARE(std::string, content_folders);` in `settings.h`.

- [ ] **Step 1: `live_content.cpp`.** `StartLiveContent` resolves each `SplitList(REXCVAR_GET(content_folders))` entry with `Resolve(entry, IniAnchor())`, runs `ScanFolders(..., kRb3TitleId, &problems)`, logs one `REXLOG_WARN` per problem and `REXLOG_INFO("content: {} packages from {}", n, folders)`, and keeps the result in a file-static vector (immutable after start). `FindLivePackage` compares `file_name` against `header.content_id` case-insensitively. Mounting:

```cpp
bool MountLivePackage(const Package& package, std::string_view root_name) {
    std::lock_guard lock(g_mutex);
    const std::string mount = fmt::format("\\Device\\Band3Content\\{}", ++g_next_mount);
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>(mount, package.path);
    if (!device->Initialize()) {
        REXLOG_WARN("content: can't read {}", package.path.string());
        return false;
    }
    const std::string link = std::string(root_name) + ":";
    g_vfs->UnregisterSymbolicLink(link);  // a root the game reuses without closing
    g_vfs->RegisterDevice(std::move(device));
    g_vfs->RegisterSymbolicLink(link, mount);
    g_mounts[ToLower(root_name)] = mount;
    return true;
}
```

`UnmountLiveRoot` looks the root up in `g_mounts` (lower-cased), unregisters the symlink and the device (`UnregisterDevice(mount)`), erases it, returns true; false if absent.

- [ ] **Step 2: band3's cases in `xam_content_hooks.cpp`.** For each override: decide from the raw registers whether the call is band3's, handle it, otherwise `sdk(ctx, base)` untouched.
  - **Enumerators:** save `content_type`, `device_id` and the `handle_out` address; call the SDK; if it returned `X_ERROR_SUCCESS` (0) and `device_id` is 0 or `DummyDeviceId::HDD` (1), look the enumerator up — `REX_KERNEL_OBJECTS()->LookupObject<rex::system::XStaticEnumerator<XCONTENT_DATA>>(handle)` for `CreateEnumerator`, `<XCONTENT_AGGREGATE_DATA>` for the aggregate one (`rex/system/xenumerator.h`) — and append one item per `LivePackagesOfType(content_type)`: `device_id = 1`, `content_type`, `set_display_name(header.display_name)`, `set_file_name(header.content_id)`; for aggregate items also `title_id = kRb3TitleId`, `xuid = 0`.
  - **CreateEx:** if `FindLivePackage(data.file_name())` with a matching content type: run a typed handler through `rex::ppc::HostToGuestFunction<Handler>(ctx, base)` with the SDK's signature (`u32 user_index, mapped_string root_name, mapped_void content_data_ptr, u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr, u32 cache_size, u64 content_size, mapped_void overlapped_ptr` — copy the types the SDK's `XamContentCreateEx_entry` uses). Open dispositions (`flags & 0xF` 3 OPEN_EXISTING, 4 OPEN_ALWAYS) mount and report disposition 2 (open) and `license_mask = header.license_mask`; the others return `X_ERROR_ACCESS_DENIED` (band3's packages are read-only). Mount failure: `X_ERROR_FILE_CORRUPT`. Complete like the SDK: with `overlapped_ptr`, `REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped, result, X_HRESULT_FROM_WIN32(result), disposition)` and return `X_ERROR_IO_PENDING`; without, return the result.
  - **Close:** `UnmountLiveRoot(root)`; if true complete (overlapped as above) with success; else the SDK's.
  - **GetCreator:** band3's package → `is_creator = 0`, `creator_xuid = 0` if the pointer is set, success; else the SDK's.
  Keep Task 3's debug logging.

- [ ] **Step 3: Wire up.** `band3_app.h` `OnPostSetup`: `band3::content::StartLiveContent(runtime()->file_system());` right after `MountGameWrites`. `config.cpp` ini row. `band3_config.ini` `[game]`:

```ini
; Folders Rock Band 3 reads DLC and custom songs (CON, LIVE) from, as they are:
; nothing is installed or unpacked. Separate folders with ';'; subfolders count.
; A relative folder is relative to this file's folder. Changes apply at the next launch.
content_folders = songs
```

README: a short `### DLC and custom songs` section under Settings saying the above, that a network folder works but a local one is safer for audio, and that a song's `song_id` written as text is corrected (Task 5).

- [ ] **Step 4: Build and check in the game.** Copy into `out/test_songs/`: `Get Got x1 v2`, `KMFDMMega_rb3con`, `letsgetitstartedfv4b_rb3con`, `Death Cab For Cutie - Transatlanticism (Tribue to Six Feet Under OST)_v1.2_rb3con`. Launch fresh with `-- --content_folders=<absolute out/test_songs> --http_enabled=true --log_file=...`, run `boot.b3t`. Expected:
  - log: `content: 4 packages from ...` (letsgetitstarted holds more than one song but is one package);
  - `GET http://127.0.0.1:21070/list_songs` lists the songs in those packages (shortnames from their `songs.dta`); record which appear;
  - start one of them on autoplay through Quickplay (see `tests/game/render_song.b3t` for the menu path, and `/jump?shortname=` to select it in the Music Library) and `wait in_game`, then `wait score>=1`: it plays;
  - quit, relaunch with the same user data: the songs are still listed and the profile loads (Review Focus 1);
  - relaunch with `--content_folders=C:/nope;<test_songs>`: a warning names `C:/nope`, the 4 packages still load (Review Focus 2).
  Then once with `--content_folders=\\BISHOP\Downloads\rb`: report the package count (expect 105) and whether a song plays without audio stutter.

- [ ] **Step 5: Commit** (all files above).

---

### Task 5: Correcting text song IDs

**Files:**
- Create: `src/Game/song_id.h`, `src/Game/song_id.cpp`
- Create: `tests/song_id_test.cpp`
- Create: `src/Hooks/song_id_hooks.cpp`
- Modify: `band3_config.toml` (two `[[midasm_hook]]` rows, next to `SongCountHook`'s)
- Modify: `tests/CMakeLists.txt`, `README.md` (one sentence in the DLC section)

**Interfaces:**
- Produces (`src/Game/song_id.h`, namespace `band3`, no SDK includes):
  - `uint32_t Crc32(std::string_view text);` standard CRC-32.
  - `int32_t CorrectedSongId(std::string_view text);` `int32_t(Crc32(text) % 9999999 + 2130000000)`.
- Consumes: `band3::Symbol` (`src/Game/Symbol.h`), `band3::DataNode`/`DataArray` layouts (`src/Game/DataNode.h`, `DataArray.h`).

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

- [ ] **Step 2: Run, see it fail. Step 3: implement** `song_id.h/.cpp` (a 256-entry table built once from polynomial `0xEDB88320`, `crc = ~0`, final `~crc`). **Step 4: tests pass.**

- [ ] **Step 5: The hooks.** RB3 reads `song_id` in two places:
  1. `GetSongID` (`0x827A87F0`, `DataArray* song` r3, `DataArray* missing_data` r4): looks `song_id` up with `DataArray::FindData<int>` (`0x8274C7F0`), in `song` first, then `missing_data` if nothing was found, and returns the int. Override it with `extern "C" REX_FUNC(GetSongID)` calling `__imp__GetSongID` (the generated original; see `src/Hooks/rb3dx.cpp` for the pattern) and then, in host code, find the `song_id` node yourself: walk the array (`DataArray.mNodes` → nodes of 8 bytes: value, type), find the child array (type `kDataArray`, 16) whose node 0 is the symbol `song_id` (`band3::Symbol(ctx, base, "song_id").value(base)`), take its node 1; check `song` first, then `missing_data`, the same order as the original. If that node is `kDataSymbol` (5) or `kDataString` (18), its value is a guest pointer to the text: return `CorrectedSongId(text)` in r3 and log once per text (`REXLOG_INFO("song_id '{}' is text; using {}", ...)`). Otherwise leave the original's result.
  2. `SongMetadata`'s constructor at `0x827AA7D4`: `addi r3,r11,8` / `bl 0x8274B0F8` (the int of the `song_id` array's node 1) / `stw r3,48(r27)`. Two midasm hooks: `SongIdNodeHook` at `0x827AA7D4`, `after_instruction = false`, `registers = ["r3"]` keeps the node's address (r3) in a `thread_local`; `SongIdValueHook` at `0x827AA7D8`, `after_instruction = false`, `registers = ["r3"]`: if the kept node is a symbol or string, set r3 to `CorrectedSongId(text)`; then clear the `thread_local`. (`0x827AA7D8` is only reached right after that call; the constructor's other path branches past it to `0x827AA7DC`.) Declare both functions as `band3_config.toml`'s existing midasm hooks are (`void Name(PPCRegister& r3)`, see `SongCountHook` in `src/patches.cpp`).
  Run codegen after editing `band3_config.toml`, then `cmake --preset` and build.

- [ ] **Step 6: Check in the game.** With `KMFDMMega_rb3con` (text ID `KMFDMMega`) and `Get Got x1 v2` (numeric `2001610165`) in a content folder, HTTP on: `GET /song_2133239510` returns KMFDM's song; `GET /song_2001610165` returns Get Got's; the log has the correction line for `KMFDMMega`. Before this task's hooks (Task 4's build) `/song_2133239510` is a 404 — record both. Relaunch with the same user data: same results (the ID is stable).

- [ ] **Step 7: Commit** (`src/Game/song_id.*`, `tests/song_id_test.cpp`, `tests/CMakeLists.txt`, `src/Hooks/song_id_hooks.cpp`, `band3_config.toml`, `README.md`).
