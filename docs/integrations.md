# Integrations

band3 implements much of [RB3Enhanced](https://github.com/RBEnhanced/RB3Enhanced)'s
behaviour natively, so tools and mods written for RB3E (including
[Rock Band 3 Deluxe](https://github.com/hmxmilohax/rock-band-3-deluxe)) work with it.
All of these are under F4 → Band3 → Integrations unless noted.

## Network events (Stage Kit, song and venue info)

Turn on `events_enabled` to send RB3Enhanced-compatible events over UDP: Stage Kit
lighting, the song, band, venue and screen, and Rock Band 3 Deluxe's
`rb3e_send_event_string` data, so existing receivers (Stage Kit lighting bridges, RB3E
dashboards) work unchanged. `events_target` is 255.255.255.255 (the local network) by
default; if receivers see nothing (common with VPN or virtual adapters), use your subnet's
broadcast address (e.g. 192.168.1.255) or one device's IP. `events_port` is 21070.
`events_target` and `events_port` need a restart.

## Discord Rich Presence

Turn on `discord_enabled` (then restart) to show the current song as Discord Rich
Presence. It talks to the Discord desktop app directly; no companion app is needed.

## Web server

Turn on `http_enabled` (then restart) and band3 serves a web page on port 21070
(`http_port`) to this PC and the local network (`http_address` 127.0.0.1 keeps it to this
PC). The log says where to open it
(`Web server: listening on 0.0.0.0:21070, open http://192.168.1.20:21070/`).

<img src="images/web-song-browser.png" alt="The web song browser" width="520">

The page lists the song library with each song's album art and the difficulty of its
parts, searchable and sortable, and **Select** highlights a song in the game's Music
Library, which has to be open. **Filters** narrows it to songs with keys, pro parts or
harmonies, under a difficulty for each part, or in some genres or decades (kept on that
device); tapping a song shows its details, and **Random** picks one of those showing. A
banner says what the game is doing: during a song, which, how far in, and the score.
Windows asks once whether to let band3 through the firewall; allow it on private networks
for other devices to reach it.

It answers RB3Enhanced's API, so RB3E's page and tools written for it work too:

| Endpoint | |
|---|---|
| `/` | the page; an `rb3e_index.html` at `game:\` replaces band3's |
| `/list_songs` | every song, as `[shortname]` sections of `shortname=`, `title=`, `artist=`, `album=` and `origin=` lines |
| `/song_<id>` | one song's lines, by song ID |
| `/jump?shortname=<name>` | selects the song in the Music Library: 409 when it isn't open, 404 for a shortname no song has |
| `/execute?script=<dta>` | runs a DTA script, only with `http_allow_scripts` on (anyone on the network could run any script, so it's off by default) |
| `/jsonrpc` | `discordrp.json` at `game:\`, which Rock Band 3 Deluxe writes for Discord presence tools |

band3 adds its own, which RB3E doesn't have:

| Endpoint | |
|---|---|
| `/song_details` | every listed song's `genre` (as the Music Library names it), `year`, `length_ms`, `vocal_parts` and `tiers`, the difficulty of each part it has (`band`, `guitar`, `bass`, `drum`, `vocals`, `keys`, `real_guitar`, `real_bass`, `real_keys`) from 0 (Warmup) to 6 (Impossible), as JSON by shortname |
| `/status` | what the game is doing, as JSON: `screen`, `in_library` (the Music Library is open, so `/jump` can select) and `playing`, during a song its `shortname`, `title`, `artist`, `score`, `position_ms` (null until the song starts) and `length_ms`, else null |
| `/album_art?shortname=<name>` | the song's album art as a JPEG, read as the game reads it for the Music Library (from the ARK, or a loose file that replaces it); 404 when the song has none, or no song has that shortname |

`http_allow_cors` adds `Access-Control-Allow-Origin: *`, for pages served from somewhere
else. Requests wait for the game's next frame, and get a 503 if it doesn't come within 5 s.
Files at `game:\` are found as the game finds them: in the `game` folder of the user data
root (see [Folders](settings.md#folders)) first, then in the game data root.

To work on the page without the game, `python tools/web_preview.py` serves it at
http://127.0.0.1:21080/ with the songs, their details and album art read from the game
data. The page comes from `src/Net/http_page.h` on every load, so an edit shows on a
refresh, without a rebuild; **Select** can't work there, and `/status` says what
`--status` (`menu`, `library` or `playing`) tells it to.

## RB3Enhanced compatibility

### Script functions

band3 gives the game's scripts RB3Enhanced's functions, so mods written for RB3E (Rock
Band 3 Deluxe among them) can call them: `rb3e_get_song_name`, `rb3e_get_artist`,
`rb3e_get_album`, `rb3e_get_genre` and `rb3e_get_origin` (each takes a song ID),
`rb3e_get_song_count`, `rb3e_set_venue` (for this session; `forced_venue` keeps its
value), `rb3e_local_ip`, `rb3e_api_version` (0, the RB3E API band3 follows),
`rb3e_build_tag`, `rb3e_commit`, `rb3e_is_emulator` (1), `rb3e_send_event_string`,
`rb3e_change_music_speed`, `rb3e_change_track_speed` and their `rb3e_get_` pairs (the
`song_speed` and `track_speed` settings, for this session) and
`print_debug`, which logs its argument. `rb3e_relaunch_game` starts band3 again with
the same command line (a minimized test run stays minimized; the new one waits for this
one to close) and closes this one. `rb3e_delete_songcache` deletes the song cache the
game mounted (`songcache`, or Deluxe's `rbdxcache`, in the user data root) the next
time band3 starts, since the game has it open. With `http_allow_scripts` on, the web
server's `/execute` runs them, e.g. `{print_debug {rb3e_get_song_name 1009}}`.

### Rock Band 3 Deluxe

Rock Band 3 Deluxe only uses those functions when its scripts see `RB3E` defined, as
RB3E's loader does. With `rb3e_mode` (on unless you turn it off, then restart) band3
defines `RB3E` and `RB3E_HAS_VERSION`: Deluxe then shows its RB3E version line, offers its
party mode (which shows this PC's address for the web page above) and looks songs up
through these functions, and its "Clear and reboot game" after a Deluxe update clears the
song cache and restarts band3.

### Modifiers and options

band3 also adds RB3E's six modifiers to the game's (Options → Modifiers, and Deluxe's
menus), with or without `rb3e_mode`: Black Background (no venue), Force HOPOs, Mirror
Mode (green and orange, red and blue swapped), Gem Color Shuffle (gems drawn in random
colours), Gem Shuffle (each chord's lanes shuffled) and Double Bass (expert drums play
the 2x bass pedal notes).

As with RB3E, keys on guitar is always unlocked: the overshell's part list offers keys to
a guitar without the career unlock. (RB3E's other always-on patches are already in: the
8000-song limit, and the song blacklist, whose check TU5 itself no longer makes.)

Two of RB3E's options are settings too (Band3 → Game, both off by default):
`unlock_clothing` unlocks every piece of clothing, tattoo and face paint and the video
venues without earning them, and `gold_on_all_difficulties` lets gold stars be earned
below expert, from the next song.
