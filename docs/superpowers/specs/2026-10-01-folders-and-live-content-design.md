# Folders, live content and song IDs

Three changes that make band3 portable and let it play DLC and custom songs straight
from a folder.

## 1. Folders (portability)

band3 keeps its files in three places, each an SDK setting:

| What | Setting | Default today |
|---|---|---|
| Game data (`default.xex`, `gen/`) | `game_data_root` | `assets`, from `band3_config.ini` |
| User data (saves, profile, `game/` writes) | `user_data_root` | `Documents\band3` |
| Cache (shaders) | `cache_root` | `<user_data_root>/cache` |

Only `game_data_root` can be set in `band3_config.ini`; the other two only on the
command line, because the SDK fixes its paths before `band3.toml` loads.

- `band3_config.ini`'s `[game]` section takes `user_data_root` and `cache_root` too.
  Empty means the default.
- The ini is looked for in the working directory, then beside the executable.
- A relative path in the ini is relative to the folder the ini is in. So
  `user_data_root = user_data` makes a portable install: everything band3 writes stays
  beside it.
- The command line still wins over the ini.

## 2. Live content folders

RB3 reads DLC and custom songs (STFS packages: `CON`, `LIVE`, `PIRS`) straight from
folders the player names, without unpacking them.

- Setting `content_folders` (Band3 → Game, restart): folders separated by `|` (no
  path can contain it, and the ini reader takes ` ;` as a comment), searched with
  their subfolders. Also `content_folders` in the ini's `[game]`. Relative paths are
  relative to the ini's folder, as above. Default: `songs`.
- At startup band3 reads the header of every file in those folders (the first
  0x511 bytes) on a worker thread, so a slow or offline network share doesn't hold up
  startup, and keeps the packages for RB3 (title `45410914`). Each package's content
  ID (header `0x32C`, 20 bytes, as 40 hex digits) is its name to the game, so the same
  package in two folders is listed once and a renamed or moved file keeps its name.
- RB3 lists DLC and custom songs through its `XContentCreateCrossTitleEnumerator`,
  which reaches XAM's aggregate enumerator by ordinal inside the SDK, so band3
  overrides that recompiled function and adds its packages to the enumerator it
  returns: once, for the shared user (255), with each package's own content type
  (custom songs are type 1, saved game; official DLC type 2, marketplace).
- The plain `XamContentCreateEnumerator` is RB3's search for saves and song caches
  (type 1); band3 never adds packages there.
- band3 supplies its own `XamContentCreateEx`, `XamContentClose` and
  `XamContentGetCreator` (Windows: the generated code calls them by symbol). Each
  handles band3's packages and passes everything else (saves, the SDK's installed
  content) to the SDK's own, which the runtime DLL exports by name.
- Opening mounts the package file read-only with the SDK's `StfsContainerDevice`;
  closing unmounts it. Creating or overwriting one of them is refused.
- Changes to the folders apply at the next launch. Linux keeps the SDK's content
  handling for now.

## 3. Song ID correction

Some custom songs give `song_id` as text (`(song_id KMFDMMega)`); RB3 reads it as a
number regardless and gets garbage that can change between launches. As RB3Enhanced
does, a text `song_id` becomes `crc32(text) % 9999999 + 2130000000` (standard CRC-32),
so IDs match RB3E's. Two places read it: `GetSongID` (`0x827A87F0`), overridden, and
`SongMetadata`'s constructor, whose `DataNode::Int` call (return address `0x827AA7D8`)
goes through band3's existing `DataNode__Int` override.
