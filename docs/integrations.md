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
| `/rv/search?text=<text>&page=<n>` | a page of 25 of [RhythmVerse](https://rhythmverse.co)'s Rock Band 3 (Xbox) songs matching the text, or its newest without; JSON with `total`, `page`, `page_size` and `songs`, each with its `file_id`, details, `tiers` as `/song_details` gives them, `song_id`, `download` (RhythmVerse hosts it, so band3 can download it), `downloaded` (its file is in the content folders), `in_library` (the game has a song with its song ID; null while the game is busy) and `update` (`available`, `pending` or empty). Optional: `sort=` `newest`, `updated`, `downloads`, `title`, `artist` or `length`; `downloadable=1` for only what band3 can download (pages of 100, the rest left out); `has=` parts (`keys`, `real_guitar`...), `harmonies=1`, `genre=` RhythmVerse's genres (`metal,rock`), `decade=1990,2000`, and `cap=<part>:<tier>` for a part's difficulty at most |
| `POST /rv/download` | downloads `{"file_id": "<id>"}` (JSON) from a search into the songs folder, or with `"update": true` the newer version of one band3 downloaded (search results' `update` is `available`), for the next launch: 404 for a song no search has found, 409 for one RhythmVerse doesn't host, or has nothing newer of |
| `/rv/downloads` | this session's downloads as JSON: the `folder` they go to, and each one's `state` (`queued`, `downloading`, `done`, `failed`), `received`, `total`, `error`, `song_id`, `update` and `in_library` (the game has taken it in; null while the game is busy, or before any is done) |

`http_allow_cors` adds `Access-Control-Allow-Origin: *`, for pages served from somewhere
else. Requests wait for the game's next frame, and get a 503 if it doesn't come within 5 s.
Files at `game:\` are found as the game finds them: in the `game` folder of the user data
root (see [Folders](settings.md#folders)) first, then in the game data root.

To work on the page without the game, `python tools/web_preview.py` serves it at
http://127.0.0.1:21080/ with the songs, their details and album art read from the game
data. The page comes from `src/Net/http_page.h` on every load, so an edit shows on a
refresh, without a rebuild; **Select** can't work there, and `/status` says what
`--status` (`menu`, `library` or `playing`) tells it to. Its RhythmVerse tab searches
RhythmVerse, but its downloads are made up and save nothing.

### RhythmVerse

The page's **RhythmVerse** tab searches [RhythmVerse](https://rhythmverse.co)'s custom
songs for Rock Band 3 on Xbox, with their art, details and difficulties, sorted as you
choose (newest, most downloaded, title...) and with **Filters** for the parts a song has,
a part's difficulty at most, genre and decade, and **Downloadable only** (on at first),
which leaves out what band3 can't download; they're kept on that device. It says which
songs you have, however you got them. **In library**: the game has a song with its
song ID (RhythmVerse's for the upload is the `song_id` in its `songs.dta`), whatever
its file is called; **Select** then selects it in the Music Library, as on the Library
tab. **Downloaded**: its file is in the content folders, under the name RhythmVerse gives
it and at its size; **Not in game yet** until the game has taken it in. **Similar in
library**: a song by its artist and title is in the game, another chart of it maybe.

**Download** saves a song's package into a `rhythmverse` folder in the first of the
content folders (`content_folders`; `songs\rhythmverse` unless you've changed it), and
the game takes it in without a restart, as it took in songs bought from the Xbox store:
band3 tells it new content is installed (the console's `XN_LIVE_CONTENT_INSTALLED`), and
the game lists its content again at its next refresh, showing "Loading New Downloaded
Content..." as it does. That's at once with the Music Library open (it keeps its place),
once a song that's playing is over, and on the way into the Music Library from the main
menu. Songs copied into the content folders by hand join the same way once a search has
seen them (the folders' files are listed again a minute on). Only songs RhythmVerse hosts
itself download this way; for those on other sites (MediaFire, Google Drive...), zipped
ones and the official DLC, **Open** goes to the song's RhythmVerse page. A download that
isn't a Rock Band package is thrown away.

band3 keeps what it downloaded in `rhythmverse.json` in the download folder, with
RhythmVerse's hashes of each upload, and when they change the song shows **Update
available**. **Update** downloads the new version beside the old one as `<name>.pending`;
the game has the old one open, so band3 puts the new one in its place when it next
starts, keeping the old one beside it as `<name>.replaced` (nothing is deleted; clear
them out when you like). Songs you got some other way aren't checked for updates.

Turn `http_rhythmverse` off (`[http] rhythmverse = false`) to leave the tab out.
RhythmVerse's API isn't documented, so a change on its side can break the tab until band3
follows.

## GoCentral (Rock Central)

Rock Band 3's online features (leaderboards, Battles, setlists shared with friends,
Rock Central's goals) talked to Harmonix's Rock Central, which closed.
[GoCentral](https://github.com/ihatecompvir/GoCentral) is a fan-run replacement that
RB3Enhanced players use. Under F4 → Band3 → Online, set `username` (Band3 → Game)
to a name of your own, turn on `gocentral`, then restart. band3 then logs into
`gocentral_address`, RB3Enhanced's Xbox 360 server (`gocentral-xbox.rbenhanced.rocks`)
unless you run your own, as RB3Enhanced does on a console with Xbox Live blocked.

GoCentral knows Xbox players by their name alone, with no password: anyone using
your name logs in as you. band3 won't connect while `username` is blank or "User",
every profile's name until you change it.

To see how a connection goes, turn on `log_net_calls` (Band3 → Debug): the log then
has each of the game's network calls and each step of its Rock Central login.

## Liveless (online play)

Rock Band 3 plays online over Xbox Live, which band3 doesn't have. RB3Enhanced's
Liveless plays without it, straight from one player's game to another's, and band3
does the same, so band3 players can play together (and, the game's packets being
the same, with RB3Enhanced players, though that's untested). Under F4 → Band3 →
Online, turn on `liveless`, set `username` to your name (others see it), then
restart.

One player hosts and the others join. Everyone presses Start for the overshell,
picks **Play on Xbox Live**, backs out of it, then picks Play Now → Quickplay →
**Find Xbox Live Players**:

- The host leaves `liveless_connect` at `127.0.0.1`: their search finds their own
  game, so they wait there for players.
- Each joining player sets `liveless_connect` to the host's address (the local
  network one on the same network, the public one over the internet). Their search
  joins the host's band.

Over the internet, as with RB3Enhanced, each player needs their `liveless_port` (UDP
9103 unless changed) forwarded to their PC and `liveless_external_ip` set to their public IP: the games tell each
other where to reach them. Only play on one PC and on a local network has been
tested so far.

Once the others show in the host's band, the host picks **Play With Current
Lineup** and everyone makes the setlist and plays together. There is no lobby or
list of games: Live's matchmaking is gone, so searching finds one game, the one at
`liveless_connect`.

Players should both search: a player who searches for a host that only went
online from the overshell (without Find Xbox Live Players) is turned away.
To join by code instead of address, see [Liveless Rooms](#liveless-rooms-joining-by-code).

`liveless_port` moves the game to another UDP port, so two band3s on one PC can
play each other: [`tests/game/liveless.b3t`](../tests/game/liveless.b3t) does that.

### Liveless Rooms (joining by code)

RB3Enhanced's Liveless Rooms joins players by an 8-character code instead of an
address: each game logs in to a Rooms server, which gives it a code, and when another
player asks for that code the server tells their game where the host's is. Under F4 →
Band3 → Online, turn on `liveless` and `liveless_rooms`, set `username` (Band3 → Game)
to your name, then restart. band3 then logs in to `liveless_rooms_server`,
RB3Enhanced's (`liveless-testing.ipg.pw`) unless you run your own. Logging in
registers your `username` there, and as with GoCentral the server knows you by that
name alone, so band3 won't log in while it's blank or "User".

Your code shows in the Rooms panel: press **F10** (`bind_liveless_rooms`), or, once
online, pick **Xbox Live Options** → **Invite Friends** in the overshell, which opens
the panel instead of Xbox Live's friends list while Rooms is on. The panel also
shows the server, the connection's state and the last thing that went wrong; with
Rooms off, it says how to turn it on. Give your code to the players who'll join you.

To join someone:

1. Go online first: press Start for the overshell, pick **Play on Xbox Live**, then
   back out of it. Until then the panel's **Join** stays greyed out ("Go online
   first: Play on Xbox Live in the overshell.").
2. Open the panel, enter the host's code (type it, or pick its characters from the
   grid under the field) and press **Join** (or Enter in the field).
3. Your game joins the host's band, as when accepting an invite on a console. Then
   you both pick the same mode: Play Now → Quickplay → **Choose Songs**. The host waits
   ("Waiting for Xbox LIVE Players...") until the player who joined picks it too.

With a controller: while the panel is open and in front, the d-pad moves around it,
A presses, X deletes a character and B closes it (B first stops typing in the field,
if A started it). The game reads no buttons until the panel closes or you click the
game, so a guitar's strum doesn't also move the overshell.

Joins by code always go to UDP port 9103, as RB3Enhanced's do, so a host keeps
`liveless_port` at 9103 (the panel warns when it isn't) and forwards it to their PC,
as RB3Enhanced players do. `liveless_external_ip` isn't needed: band3 tells the game
its public address as the Rooms server saw it, unless `liveless_external_ip` is set,
which wins. A player joins the host's public address, or its local network one when
both have the same public address (the same network, or one PC). When a player
joins, the server also has the host's game send them a packet first, which may get
the join through some routers without the forward; don't count on it.

What the panel's errors mean:

| Error | |
|---|---|
| `no IPv4 address for <server>` | `liveless_rooms_server` didn't resolve: check it and the PC's connection |
| `no answer from <server>`, `<server> refused the connection` | nothing is taking connections at that address (port 19532 unless it has `:port`), or a firewall is in the way |
| `the server turned the connection away` | the server isn't taking this client |
| `login refused: check that liveless_rooms_server matches the server's address, and username` | the server closed the connection before giving a code. It checks the login against its own name, so `liveless_rooms_server` must be that name as the server knows it (not, say, its IP address) |
| `the server closed the connection`, `the server stopped answering`, `bad data from the server` | the connection went after logging in (the server sends nothing for 30 s, or something it can't read). band3 doesn't connect again by itself: press **Connect** |
| `not logged in to the Rooms server` | a join before the code came, or after the connection went |
| `a code is 8 letters and digits` | the code isn't 8 characters long |
| `no game with code <code>` | no one is logged in with that code: check it with the host, whose game must be running with Rooms on |
| `join denied (reason <n>)` | the server turned the join away for a reason band3 doesn't know |
| `the server gave no address for <user>` | the server found the host but gave no address to reach them at |

The log's `rooms:` lines follow the connection: logging in, with the code and public
address, and what each join did.

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
