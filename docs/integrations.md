# Integrations

band3 implements much of [RB3Enhanced](https://github.com/RBEnhanced/RB3Enhanced)'s
behaviour natively, so tools and mods written for RB3E (including
[Rock Band 3 Deluxe](https://github.com/hmxmilohax/rock-band-3-deluxe)) work with it.
All of these are on the Online tab, on the launcher or in the in-game settings (in their All settings, Band3 →
Online), unless noted.

## Network events (Stage Kit, song and venue info)

Turn on `events_enabled` to send RB3Enhanced-compatible events over UDP: Stage Kit
lighting, the song, band, venue and screen, and Rock Band 3 Deluxe's
`rb3e_send_event_string` data, so existing receivers (Stage Kit lighting bridges, RB3E
dashboards) work unchanged. `events_target` is 255.255.255.255 (the local network) by
default; if receivers see nothing (common with VPN or virtual adapters), use your subnet's
broadcast address (e.g. 192.168.1.255) or one device's IP. `events_port` is 21070.
`events_target` and `events_port` need a restart.

## Stage Kit lights

band3 lights Stage Kits itself, as YARG does, so a kit needs no RB3E Dashboard or
bridge in between. The game's lighting goes to every kit band3 can reach; the
**Lights** tab (in the launcher and the in-game settings) lists them and tests them.

- **Plugged in by USB** (`stagekit_usb`, on by default): Santroller Stage Kits, which a
  PC sees in HID mode (VID 1209, PID 2882; band3 sends them the HID report the RB3E
  Dashboard's Pico firmware does), and Xbox 360 Stage Kits (XInput subtype 9; the
  rumble the game sends). band3 looks for them every 2 seconds, so they can come and
  go, and turns them off when a song ends and when band3 closes. A Stage Kit isn't a
  player: it doesn't take a controller's place or play as a guitar. An XInput kit does
  take one of Windows' four XInput slots, shared with Xbox 360 instruments, so with four
  of those plugged in there's none left for it; a Santroller kit in HID mode takes no
  slot (it shows as HID unless its config sets XInput on Windows).
- **Wireless, Pico W** (`pico_discovery`, on by default): the
  [RB3E Dashboard's](https://github.com/idio-sync/rb3e-stagekit-networked) Pico W
  firmware. band3 finds the Picos as the dashboard does (discovery on UDP 21071 every 5
  seconds) and lists each with its signal and whether a kit is plugged into it; a
  quiet one shows offline after 10 seconds and leaves the list after 30. A Pico answers
  whichever program found it last, so turn this off while the dashboard itself runs;
  and if the dashboard on this PC holds port 21071, the tab says so. Picos follow the
  game through the network events above, so `events_enabled` must be on; the tab
  offers to turn it on when it finds Picos with it off.

The tab's test controls are the dashboard's, for one device or every device: **Test
lights** (each colour in turn, then off), fog on and off, strobe slow, fast and off,
all off, one colour's LEDs one at a time or in a pattern (all, none, odds, evens, left,
right), and a chase. A song's next lighting command takes over from a test.

To see what band3 sends a Pico without one, `python tools/fake_pico.py` stands in for
a Pico on this PC and prints the commands it gets.

## Home Assistant

band3 can tell [Home Assistant](https://www.home-assistant.io) what the game is doing,
as the RB3E Dashboard (`rb3e-stagekit-networked`) does, without running the dashboard.
It does it two ways, and you can use either or both:

- **MQTT**: band3 connects to Home Assistant's MQTT broker and shows up as a device,
  "band3 (*your PC's name*)", with entities for the song, artist, venue, score, screen,
  band and more, which Home Assistant finds by itself (MQTT discovery).
- **Webhook**: band3 POSTs the dashboard's webhook payloads when a song starts, ends or
  changes, so an automation written for the dashboard works unchanged.

Home Assistant only watches: nothing it sends changes the game. The settings are in the
Online tab's Home Assistant section, and in `band3.toml` as the `ha_*` keys below
(`ha_mqtt_host = "192.168.1.10"`, at the top level like every other setting there); the
older `band3_config.ini` has them in `[homeassistant]`, without the `ha_` (`mqtt_host`,
`mqtt_port`, ...). All of them need a restart.

### Setting up MQTT

1. In Home Assistant, install the **Mosquitto broker** add-on (Settings → Add-ons → Add-on
   store) and start it, then add the **MQTT** integration (Settings → Devices & services),
   which usually finds the broker by itself.
2. Make a Home Assistant user for band3 (Settings → People → Users). The Mosquitto add-on
   accepts Home Assistant's users.
3. In band3's Online tab, set **MQTT broker** (`ha_mqtt_host`) to Home Assistant's
   address (e.g. `homeassistant.local` or `192.168.1.10`), then **User name**
   (`ha_mqtt_username`) and **Password** (`ha_mqtt_password`) to that user's. **Port**
   (`ha_mqtt_port`, 1883) and **Discovery prefix** (`ha_discovery_prefix`,
   `homeassistant`) only change if you changed them in Home Assistant.
4. Restart band3. The device appears under the MQTT integration.

The password is stored as plain text in `band3.toml`. The launcher and the in-game
settings mask it, but the SDK's own menu (the in-game settings' **All settings...**)
shows it. band3 never writes it to the log or to `/status`.

### Entities

Each entity's state is on `band3/<id>/<entity>`, where `<id>` is the PC's name in lower
case with everything but letters and digits made `_` (so two PCs can share one broker).
Home Assistant names them after the device, e.g. `sensor.band3_desktop_abc_song` on a
PC named DESKTOP-ABC.

| Entity | Kind | State |
|---|---|---|
| Song | sensor | the song's title; attributes `shortname`, `artist` and `length_ms` |
| Artist | sensor | the song's artist |
| Venue | sensor | the venue's name as the game has it |
| Score | sensor | the band's score |
| Playing | binary sensor (running) | on during a song, until you leave its results |
| Paused | binary sensor | on while the song is paused |
| Screen | sensor | the game's name for the screen that's up, e.g. `main_hub_screen` |
| Song progress | sensor | how far into the song, 0-100 %, at most once a second; attributes `position_ms` and `length_ms` |
| Band | sensor | each player's part and difficulty, e.g. `Guitar (Expert) · Drums (Hard)` |

The song, artist and score keep the last song's after it ends, and progress the point
the song ended at.

With **Stage Kit lights** (`ha_stagekit`) on, band3 also tells Home Assistant what the
game sends to a Stage Kit, as it sends it:

| Entity | Kind | State |
|---|---|---|
| Stage Kit red, yellow, green, blue | sensor | how many of the colour's 8 LEDs are lit; attribute `mask`, which ones (0-255) |
| Stage Kit strobe | sensor | `off`, or its speed, `1` to `4` |
| Stage Kit fog | binary sensor | on while the fog machine is |

These change many times a second during a song (band3 sends each at most every 50 ms), so
keep them out of Home Assistant's recorder, in `configuration.yaml`:

```yaml
recorder:
  exclude:
    entity_globs:
      - sensor.band3_*_stage_kit_*
      - binary_sensor.band3_*_stage_kit_*
```

Turning Stage Kit lights off removes these entities from Home Assistant the next time
band3 connects.

The entities are unavailable while band3 isn't running: band3 says "offline" on
`band3/<id>/status` when it closes, and the broker says it for band3 if it crashes or its
window is closed (it's the connection's last will). The states are retained on the
broker, so when Home Assistant restarts while band3 runs it has them again at once (an
empty one, such as the song before any was played, shows as unknown).

### Connection state

The web server's `/status` has the connection's state in `ha_state` (and the
[test harness](test-harness.md)'s `state` too, unless it's `off`):

| `ha_state` | |
|---|---|
| `off` | no MQTT broker set |
| `connecting` | connecting to the broker |
| `connected` | connected; the entities are up to date |
| `retrying in N s` | the broker couldn't be reached or the connection dropped; band3 tries again at once, then 5, 10, 20 and 40 s apart, then every 60 s. Only a connection that lasted a minute starts these over, so a broker that keeps taking band3 and dropping it (another client with its id, an ACL) is retried ever more slowly too |
| `refused: <reason>` | the broker turned band3 away for a reason only a settings change mends (`bad user name or password`, `not authorized`); band3 doesn't try again until it restarts |

The log's `ha:` lines say when band3 connects, why it couldn't or the connection dropped
(once per distinct error, not on every retry), and what went wrong with the webhook.

### Webhook

Set **Webhook URL** (`ha_webhook_url`) to a Home Assistant webhook,
`http://<home assistant>:8123/api/webhook/<webhook id>`, and restart band3. It POSTs
these as JSON, as the RB3E Dashboard does:

| When | Payload |
|---|---|
| a song starts | `{"type":"state","status":"playing"}` |
| a song ends (you leave its results) | `{"type":"state","status":"menu"}` |
| the song changes | `{"type":"song","name":"<title>"}`, sent before `playing` when a song starts |

The dashboard's `HomeAssistant/rb3e_lighting.yaml` automation works unchanged with webhook
id `rb3_event`; a copy is in [home-assistant/rb3e_lighting.yaml](home-assistant/rb3e_lighting.yaml)
(it also puts the song's title in an `input_text` helper). A shorter one that dims a light
for each song:

```yaml
alias: "Rock Band 3 lights"
trigger:
  - platform: webhook
    webhook_id: "rb3_event"
    local_only: true
    allowed_methods:
      - POST
action:
  - choose:
      - conditions: "{{ trigger.json.type == 'state' and trigger.json.status == 'playing' }}"
        sequence:
          - service: light.turn_on
            target:
              entity_id: light.living_room  # your light
            data:
              brightness_pct: 30
      - conditions: "{{ trigger.json.type == 'state' and trigger.json.status == 'menu' }}"
        sequence:
          - service: light.turn_on
            target:
              entity_id: light.living_room
            data:
              brightness_pct: 100
mode: queued
```

Each POST has 2 s and isn't retried: a late "playing" is worse than none. A failure (no
answer, or a status other than 2xx) is logged once per distinct error
(`ha: webhook: HTTP 404`). The webhook doesn't need MQTT; with both set, both run.

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
**Duplicates** lists the songs that are in more than one package, or clash with the game's
own (see [Duplicate songs](#duplicate-songs)).
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
| `/library/duplicates` | the [duplicate songs](#duplicate-songs) as JSON: `reading` (the packages are being read; ask again), `read` and `total` packages, `unreadable` (packages whose songs.dta couldn't be read), `game` (the game's own songs were compared; false while it's busy, and just after it starts, until it has loaded them) and `groups`, each a `kind` (`same_file`, `song_id`, `shortname` or `similar`), its `key` and its `copies`: `shortname`, `song_id`, `title`, `artist`, `file` (the package; empty for the game's own songs), `songs_in_file`, `size`, `in_use` and `differs` (a copy band3 left out that isn't the size of the one it loads); and `set_aside`, each a `file` and `next_launch` (not done yet) |
| `POST /library/set_aside` | sets a package aside at the next launch: `{"file": "<path>"}`, one of the report's with one song or a copy band3 left out, or `{"left_out": true}` for every such copy left out (but those of another size); 409 for anything else |
| `POST /library/put_back` | `{"file": "<path>"}`, one of the report's `set_aside`: cancels it if it's waiting for the next launch, or renames it back |
| `/status` | what the game is doing, as JSON: `screen`, `in_library` (the Music Library is open, so `/jump` can select), `ha_state` (the [Home Assistant](#connection-state) connection) and `playing`, during a song its `shortname`, `title`, `artist`, `score`, `position_ms` (null until the song starts) and `length_ms`, else null |
| `/album_art?shortname=<name>` | the song's album art as a JPEG, read as the game reads it for the Music Library (from the ARK, or a loose file that replaces it); 404 when the song has none, or no song has that shortname |
| `/rv/search?text=<text>&page=<n>` | a page of 25 of [RhythmVerse](https://rhythmverse.co)'s Rock Band 3 (Xbox) songs matching the text, or its newest without, as JSON (`total`, `page`, `page_size`, `songs`). Each song has its `file_id`, details, `tiers` (as `/song_details`), `song_id`, `download` (band3 can download it), `downloaded` (its file is in the content folders), `in_library` (null while the game is busy) and `update` (`available`, `pending` or empty). Optional: `sort=` `newest`, `updated`, `downloads`, `title`, `artist` or `length`; `downloadable=1`; `has=` parts (`keys`, `real_guitar`...); `harmonies=1`; `genre=metal,rock`; `decade=1990,2000`; `cap=<part>:<tier>` for a part's difficulty at most |
| `POST /rv/download` | downloads `{"file_id": "<id>"}` from a search into the songs folder, or with `"update": true` the newer version of one band3 downloaded: 404 for a song no search has found, 409 for one RhythmVerse doesn't host, or has nothing newer of |
| `/rv/downloads` | this session's downloads as JSON: the `folder` they go to, and each one's `state` (`queued`, `downloading`, `done`, `failed`), `received`, `total`, `error`, `song_id`, `update` and `in_library` |
| `/rv/updates` | newer versions on RhythmVerse of the songs band3 downloaded, as JSON: `checking` (a check is running), `checked` (when the last one finished, in seconds since 1970; 0 for never), `error` (why the last one failed), `downloads` (how many songs band3 has downloaded) and `updates`, songs as `/rv/search` gives them, with `update` `available`, or `pending` once downloaded |
| `POST /rv/check` | checks RhythmVerse for those updates now (send `{}` as JSON); `/rv/updates` says when it's done |

`http_allow_cors` adds `Access-Control-Allow-Origin: *`, for pages served from somewhere
else. Requests wait for the game's next frame, and get a 503 if it doesn't come within 5 s.
Files at `game:\` are found as the game finds them: in the `game` folder of the user data
root (see [Folders](settings.md#folders)) first, then in the game data root.

To work on the page without the game, `python tools/web_preview.py` serves it at
http://127.0.0.1:21080/ with the songs, their details and album art read from the game
data. The page comes from `src/Net/http_page.h` on every load, so an edit shows on a
refresh, without a rebuild; **Select** can't work there, and `/status` says what
`--status` (`menu`, `library` or `playing`) tells it to. Its RhythmVerse tab searches
RhythmVerse, but its downloads are made up and save nothing, and its updates are two
made-up songs.

### Karaoke and live pages

`/karaoke` is a sing-along page for a screen facing the room: the song's lyrics, the line
being sung and the next, each syllable filling as it's sung, with the harmony parts
stacked when the band's vocalist sings harmonies. Between songs it shows the song's album
art, title and artist. Its corner button sets the timing (for a TV's delay) and the text
size, kept in that browser. A page over plain http can't keep a device awake, so turn its
sleep off. The library page's header links to it.

It's built on two endpoints other pages can use:

| Endpoint | |
|---|---|
| `/lyrics?shortname=` | the song's lyrics from its MIDI file: `{"shortname", "parts": [{"part": "lead" \| "harm1" \| "harm2" \| "harm3", "lines": [{"start_ms", "end_ms", "syllables": [{"start_ms", "end_ms", "text", "join", "spoken"}]}]}]}`; 404 for a song without vocals, 503 while the game is busy |
| `/live/events` | server-sent events: `state` (`screen`, `in_game`, `paused`, `song` {`shortname`, `title`, `artist`, `length_ms`} or null, `vocals` `none` \| `lead` \| `harmonies`) whenever it changes, and `clock` (`song_ms`, the game's song clock) four times a second during a song. Four streams at once: a fifth takes the place of the one open longest (a device asleep can keep its connection open), which gets `evicted` and stops; its page offers a tap to follow the game again |

`tools/web_preview.py` serves `/karaoke` without the game, over a made-up show
(`--karaoke-song <shortname>` for a song's own lyrics, `--vocals harmonies`).

### Duplicate songs

The same song often ends up in more than one package: a single and a pack, two versions
of a chart, a download and a copy made by hand. The Library tab's **Duplicates** reads
the `songs.dta` of every package band3 has (in the background, the first time, then only
new and changed packages; band3 keeps what it read in `band3_package_songs.json` in the
user data root) and lists four kinds:

- **Same file**: copies of one package, in two folders or under two names. Its content ID
  hashes its header and, through it, its contents, so band3 loads only the first it finds
  and the rest only take up space. A copy that isn't the size of the one loaded says
  **Another size**: look before setting it aside.
- **Same song ID**: the game takes the first of these it loads, in band3's order (the
  content folders' order, then by file name), and leaves the rest out without a word.
  The one it has is **In use**, the rest **Left out**.
- **Same shortname**, with different song IDs: the game has them all, but what finds a
  song by its shortname, as **Select** does, finds only one. A custom song that takes a
  disc song's shortname shows here too.
- **Same artist and title**, by letters and digits: other charts of the same song, maybe,
  which the game has all of.

**Set aside** takes a copy out: when band3 next starts, it renames the file to end in
`.setaside`, which it passes over (the game may have the file open until then, so it
waits, leaving `<name>.setaside-next` beside it to say so). Only a package that's a song
on its own can be set aside, never a pack, whose other songs would go with it, and never
the game's own songs; a copy under **Same file** that band3 leaves out can, pack or not,
as the one it loads has all its songs. **Set aside the left-out copies** does it for every
copy left out already (but those of another size), so nothing changes in the game; setting aside the copy that's **In
use** lets the next one take its place. Nothing is deleted: what's set aside is listed at
the end, with **Cancel** for what's waiting for the next launch and **Put back**, which
renames a file back for the next launch. A song band3 downloaded from RhythmVerse shows
**Download** again once it's set aside.

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
content folders (`songs\rhythmverse` unless you've changed `content_folders`), and the game
takes it in without a restart, as it took in songs bought from the Xbox store (the
console's `XN_LIVE_CONTENT_INSTALLED`, then "Loading New Downloaded Content..."): at once
with the Music Library open, once a song that's playing is over, or on the way into the
Music Library. Songs copied into the content folders by hand join the same way once a
search has seen them. Only songs RhythmVerse hosts itself download this way; for those on
other sites, zipped ones and the official DLC, **Open** goes to the song's RhythmVerse
page. A download that isn't a Rock Band package is thrown away.

band3 keeps what it downloaded in `rhythmverse.json` in the download folder, with
RhythmVerse's hashes of each upload, and when they change the song shows **Update
available**. **Update** downloads the new version beside the old one as `<name>.pending`;
the game has the old one open, so band3 puts the new one in its place when it next
starts, keeping the old one beside it as `<name>.replaced` (nothing is deleted; clear
them out when you like). Songs you got some other way aren't checked for updates.

band3 checks for those updates by itself, half a minute after it starts, unless it
checked in the last 12 hours, and **Check for updates** at the top of the tab checks now.
Rather than look up each song, it reads RhythmVerse's list of uploads by when they last
changed, newest first, back to its last check (or the oldest download, if that's later),
less a day for RhythmVerse's time zone: a request or two, however many songs you have,
with a pause between them. Songs with a newer version are listed above RhythmVerse's songs,
and counted on the tab, with **Update** on each and **Update all**. A search that shows a
song you downloaded notes its version as well. Without anything band3 downloaded, nothing
is checked.

Turn `http_rhythmverse` off (`[http] rhythmverse = false`) to leave the tab out.
RhythmVerse's API isn't documented, so a change on its side can break the tab until band3
follows.

## GoCentral (Rock Central)

Rock Band 3's online features (leaderboards, Battles, setlists shared with friends,
Rock Central's goals) talked to Harmonix's Rock Central, which closed.
[GoCentral](https://github.com/ihatecompvir/GoCentral) is a fan-run replacement that
RB3Enhanced players use. On the Online tab, set `username` (the Game tab's Profile name)
to a name of your own, turn on `gocentral`, then restart; or do it on the
[launcher](settings.md#the-launcher)'s Game and Online tabs and press Play. band3 then logs into
`gocentral_address`, RB3Enhanced's Xbox 360 server (`gocentral-xbox.rbenhanced.rocks`)
unless you run your own, as RB3Enhanced does on a console with Xbox Live blocked.

GoCentral knows Xbox players by their name alone, with no password: anyone using
your name logs in as you. band3 won't connect while `username` is blank or "User",
every profile's name until you change it.

To see how a connection goes, turn on `log_net_calls` (the in-game settings' Advanced tab, Logging): the log then
has each of the game's network calls and each step of its Rock Central login.

## Liveless (online play)

Rock Band 3 plays online over Xbox Live, which band3 doesn't have. RB3Enhanced's
Liveless plays without it, straight from one player's game to another's, and band3
does the same, so band3 players can play together (and, the game's packets being
the same, with RB3Enhanced players, though that's untested). In the in-game settings'
Online tab, turn on `liveless`, set `username` (the Game tab's Profile name) to your name (others
see it), then restart; or set them on the [launcher](settings.md#the-launcher)'s Online
and Game tabs and press Play.

One player hosts and the others join. Everyone presses Start for the overshell,
picks **Play on Xbox Live**, backs out of it, then picks Play Now → Quickplay →
**Find Xbox Live Players**:

- The host leaves `liveless_connect` at `127.0.0.1`: their search finds their own
  game, so they wait there for players.
- Each joining player sets `liveless_connect` to the host's address (the local
  network one on the same network, the public one over the internet). Their search
  joins the host's band.

Over the internet, as with RB3Enhanced, each player needs their `liveless_port` (UDP
9103 unless changed) forwarded to their PC, and the games tell each other where to
reach them. band3 asks the router for both the forward and the public address itself
(see [Port mapping](#port-mapping)); forward the port by hand and set
`liveless_external_ip` to your public IP only when that doesn't work. Only play on one
PC and on a local network has been tested so far.

Once the others show in the host's band, the host picks **Play With Current
Lineup** and everyone makes the setlist and plays together. There is no lobby or
list of games: searching finds one game, the one at `liveless_connect`. The host has to
search too: a host that only went online from the overshell turns joining players away.
To join by code instead of address, see [Liveless Rooms](#liveless-rooms-joining-by-code).

`liveless_port` moves the game to another UDP port, so two band3s on one PC can
play each other: [`tests/game/liveless.b3t`](../tests/game/liveless.b3t) does that.

### Port mapping

With `liveless` on, band3 asks the router to forward `liveless_port` to this PC, as
RB3Enhanced does, so players over the internet reach your game without a forward made
by hand: by PCP first, then NAT-PMP if the router doesn't speak PCP, then UPnP. The
router also says its public address, which band3 tells players joining you unless
`liveless_external_ip` is set or the Rooms server saw another (the order is
`liveless_external_ip`, the Rooms server's, the router's, then this PC's on the local
network). A router that gives a private address (10.x, 172.16-31.x, 192.168.x,
100.64-127.x: it's behind another router) still forwards the port, but band3 doesn't
tell players that address. The mapping lasts an hour, renewed every half hour, and
band3 deletes it when it closes. Turn `liveless_port_mapping` off (the in-game settings' Online tab, More settings; then
restart) to leave the router alone.

band3 asks when it starts, and a mapping that failed isn't asked for again during the
session: if the router or the network comes up after band3 starts, restart band3. A
crash or a killed band3 can leave the mapping behind: a leased one lapses within the
hour, but a router that only keeps permanent mappings holds it until band3's next clean
exit. On its next start band3 deletes that old mapping if the router won't replace it
(UPnP's ConflictInMappingEntry); a mapping of the port to another PC, or by another
program, it leaves alone, and says whose it is.

The log's `port mapping:` lines say how it went, and the Rooms panel (F10) shows it
under your code: `UDP 9103: mapped by PCP, public 203.0.113.5`, `mapping...`, or
`not mapped:` and why. When it isn't mapped, forward the port by hand and set
`liveless_external_ip`, as RB3Enhanced players do. A router can only forward to one PC,
so two PCs behind one router can't both use 9103.

The router's forward doesn't open Windows Firewall: if you declined its prompt when
band3 first went online, allow band3.exe for UDP in Windows Security → Firewall &
network protection → Allow an app through firewall.

### Liveless Rooms (joining by code)

RB3Enhanced's Liveless Rooms joins players by a room code instead of an
address: each game logs in to a Rooms server, which gives it a code, and when another
player asks for that code the server tells their game where the host's is. In the in-game
settings' Online tab, turn on `liveless` and `liveless_rooms` (More settings), set `username` (the Game tab)
to your name, then restart. band3 then logs in to `liveless_rooms_server`,
RB3Enhanced's (`liveless-testing.ipg.pw`) unless you run your own. Logging in
registers your `username` there, and as with GoCentral the server knows you by that
name alone, so band3 won't log in while it's blank or "User". Codes accept 1-8 letters
and digits, in either case; the public server currently gives five-character codes.

Your code shows in the Rooms panel: press **F10** (`bind_liveless_rooms`), or, once
online, pick **Xbox Live Options** → **Invite Friends** in the overshell, which opens
the panel instead of Xbox Live's friends list while Rooms is on. The panel also
shows the server, the connection's state and the last thing that went wrong; with
Rooms off, it says how to turn it on. Give your code to the players who'll join you.

When the connection to the server goes, or no server answers, band3 connects again
by itself: 5 s later, then 10, 30 and 60 s apart until it's back (the panel's state
says "reconnecting in N s"), starting over at 5 s once it logs in again. **Connect**
connects now instead. It doesn't try again when the server itself said no (turned the
connection away, or refused the login): that takes a change on your side.

To join someone:

1. Go online first: press Start for the overshell, pick **Play on Xbox Live**, then
   back out of it. Until then, and again once the game leaves Xbox Live, the panel's
   **Join** stays greyed out ("Go online first: Play on Xbox Live in the overshell.").
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
`liveless_port` at 9103 (the panel warns when it isn't), forwarded to their PC by band3
([Port mapping](#port-mapping)) or by hand. `liveless_external_ip` isn't needed: band3
tells the game its public address as the Rooms server saw it. A player joins the host's
public address, or its local network one when both have the same public address. The
server also has the host's game send the joining player a packet first, which may get the
join through some routers without the forward; don't count on it.

What the panel's errors mean (✓: band3 connects again by itself, as above):

| Error | | |
|---|---|---|
| `no IPv4 address for <server>` | `liveless_rooms_server` didn't resolve: check it and the PC's connection | ✓ |
| `no answer from <server>`, `<server> refused the connection` | nothing is taking connections at that address (port 19532 unless it has `:port`), or a firewall is in the way | ✓ |
| `the server turned the connection away` | the server isn't taking this client | |
| `login refused: check that liveless_rooms_server matches the server's address, and username` | the server closed the connection before giving a code. It checks the login against its own name, so `liveless_rooms_server` must be that name as the server knows it (not, say, its IP address): fix the setting and restart | |
| `the server closed the connection` | the connection went after logging in; the code may change | ✓ |
| `the server stopped answering` | the server sent nothing for 30 s | ✓ |
| `bad data from the server` | the server sent something band3 can't read. Before logging in, the address isn't a Rooms server | ✓ after logging in |
| `not logged in to the Rooms server` | a join before the code came, or after the connection went | |
| `the game isn't online yet: Play on Xbox Live first` | a join while the game is offline: before Play on Xbox Live, or after leaving it |
| `a code is 1-8 letters and digits` | the code is empty, longer than eight characters, or contains something other than ASCII letters and digits |
| `no game with code <code>` | no one is logged in with that code: check it with the host, whose game must be running with Rooms on |
| `join denied (reason <n>)` | the server turned the join away for a reason band3 doesn't know |
| `the server gave no address for <user>` | the server found the host but gave no address to reach them at |

The log's `rooms:` lines follow the connection: logging in, with the code and public
address, and what each join did. band3 tells the server which build it is
(`band3 <build>`, the build its log's `band3 build` line names), as RB3Enhanced does.

## RB3Enhanced compatibility

### Script functions

band3 gives the game's scripts RB3Enhanced's functions, so mods written for RB3E (Rock
Band 3 Deluxe among them) can call them: `rb3e_get_song_name`, `rb3e_get_artist`,
`rb3e_get_album`, `rb3e_get_genre` and `rb3e_get_origin` (each takes a song ID),
`rb3e_get_song_count`, `rb3e_set_venue` (for this session; `forced_venue` keeps its
value), `rb3e_local_ip`, `rb3e_api_version` (0, the RB3E API band3 follows),
`rb3e_build_tag` (`band3 <build>`), `rb3e_commit`, `rb3e_is_emulator` (1), `rb3e_send_event_string`,
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

Two of RB3E's options are settings too (the Game tab, both off by default):
`unlock_clothing` unlocks every piece of clothing, tattoo and face paint and the video
venues without earning them, and `gold_on_all_difficulties` lets gold stars be earned
below expert, from the next song.

### Song source icons

As RB3E does (its `GameOriginIcons`), band3 shows an icon at the left of each song in
the song list for the game or pack the song came from: Rock Band 3, Rock Band 2, The
Beatles, a track pack, customs and so on, from the song's `game_origin`. It's on unless
`game_origin_icons` (the Game tab) is off, and needs two things band3 doesn't ship:

- the icons, `ui/resource/game_origins/<origin>.png`, which Rock Band 3 Deluxe has (over
  a hundred of them). A song whose origin has no icon shows none.
- a song list with a mesh slot named `game_origin_icon` in its rows, which neither the
  game's nor Deluxe's has yet. RB3E's edit of the game's
  `ui/resource/list/gen/list_song_select_browser.milo_xbox` adds one; put it in the game
  data folder at that path, as a [loose file](settings.md#loose-files-mods).

The log says how many origins' icons it loaded each time the song list opens, and with
`log_level = debug`, the slots the loaded song list has. Turning the setting off takes the
icons away the next time the list redraws.
