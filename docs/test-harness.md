# Test harness

The game tests in `tests/game/` and the [render checks](native-renderer.md#render-checks)
run on this harness. They need the game, so unlike the [unit tests](building.md#checks)
they don't run in CI.

`tools/band3ctl.py` drives a running band3 from a script or a terminal: it presses
the virtual instrument's buttons, waits for screens, reads the game's state and takes
screenshots. With `test_port` set, band3 takes its commands on that port, on this
machine only, and connects the virtual instrument as player 1.

```
python tools/band3ctl.py launch --fresh       # start band3 minimized and muted, on a fresh test profile
python tools/band3ctl.py state                # screen, song, venue, band, frame count
python tools/band3ctl.py press green+strum_down
python tools/band3ctl.py wait screen=splash_screen timeout=60s
python tools/band3ctl.py screenshot menu      # saved under screenshots/ next to the exe
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py "hold up+orange; wait frames=60; pad; release all"
python tools/band3ctl.py window offscreen     # restore the window off every monitor, unfocused
python tools/band3ctl.py window onscreen      # on the primary monitor, unfocused: the user sees it
python tools/band3ctl.py window shot out.png  # what the window presents, overlays and all
```

A `.b3t` script is these commands one per line, with `#` comments. `run` stops at the first
one that fails, saves a screenshot of the moment and exits 1. Commands joined with `;` share
one connection, which `hold` needs: band3 lets go of everything held when a client
disconnects.

**`launch`** starts the game minimized without taking focus and muted, with its own saves in
`out/test_user_data`, so tests never touch your profile.

| Option | |
|---|---|
| `--show`, `--sound` | watch it, hear it |
| `-- --launcher` | shows the launcher, which `test_port` otherwise skips; `launch` waits for the window, and the `window` commands drive it. `--no-harness -- --launcher` shows it without `test_port` at all |
| `--config <file>` | uses that file as the `band3.toml` beside the exe, putting back what was there (moved to `band3.toml.band3ctl`) once the harness answers. Refuses `--no-harness` and `--launcher`, which would save into the player's own file. The command line's `--user_data_root` wins over the file's |
| `-- --renderer=emulated` or `both` | the game starts on its default renderer (on Windows the [native renderer](native-renderer.md) alone, where `screenshot emulated` is an error). Under `both`, `bind renderer` (F8) switches the picture and `screenshot`'s reply says which it took. `set renderer` switches between `emulated` and `both` at once; to or from `native` it applies at the next launch |

`python tools/test_config_file.py` uses `--config` to check that a `band3.toml` from the
launcher's writer reaches the game and wins over `band3_config.ini`. It needs the unit tests
built (for `band3_write_config`).

**`window`** works on the game's window itself, not through the harness, and never activates
it. A minimized window never paints, so `window offscreen` restores it off to the right of
every monitor, where it paints unseen. `window onscreen` moves it to the primary monitor, for
pacing a real vertical blank drives. **That puts the game on the user's screen**: ask first on
a machine someone is using, and `window offscreen` or `window minimize` it once done.

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms] [until <condition> [every=2s] [timeout=30s]]` | press and let go (100 ms); join inputs with `+`. With `until`, waits for the condition (as `wait` does) and presses again every `every` while the screen hasn't changed, for a press RB3 dropped |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `midi <hex bytes> [until <condition> [every=2s] [timeout=30s]]` | hands the [MIDI keyboard](#midi-keyboards) driver messages as its port would: `midi 90 3c 64` is a note-on. Each message starts with its status byte, so one command can hold several: `midi 90 30 64 80 30 00` taps the lowest C (a press is held at least 30 ms, a menu button 100 ms). With `until`, sends them all again every `every` while the screen hasn't changed, as `press` does. Fails unless `midi_keys` is on |
| `state` | the screen, whether a song is `paused`, song, venue, band, frame count, each player's instrument, the band's `score` (0 when a song starts), `ha_state` (the [Home Assistant](integrations.md#connection-state) connection, left out while it's `off`) and, while `usb_mics` records, the `mics` slots: what records each, whether the game connected it, the bytes `fed` to it and, while band3 plays it to the speakers (`usb_mic_monitor`), the `monitor` volume (0 while the game wouldn't play it back) |
| `pad [player]` | the buttons, triggers and sticks the game reads from a player (1-4) |
| `wait <condition> [timeout=30s]`, `expect <condition> [timeout=5s]` | `screen=`, `screen~` (contains), `in_game`, `menus`, `song=<shortname>`, `frames=<n>`, `score>=<n>`, `mic=<slot>` (connected and fed audio since the wait began), `rooms=<off\|connecting\|connected\|logged_in\|disconnected\|failed>` (the Liveless Rooms connection), `port_mapping=<off\|searching\|mapped\|failed>` (Liveless' port mapping), `joined` (an online band formed with this game in it; `state` then shows `"joined":true`), `paused` (the song is paused, by its pause menu or the game) |
| `sleep <n>s\|<n>ms` | waits that long, up to 600 s. `frames=` counts the main thread's frames, which stand still during the boot logos and pass twice as fast at `refresh_rate` 120, so a pause for a menu to settle is a `sleep` |
| `screenshot [emulated\|native] [name]` | the picture the window shows: the emulated GPU's at the game's size, or the native renderer's next frame at the size it draws at (minimized too). `emulated` or `native` takes that one whatever the window shows; `native` with the native renderer off draws the next frame once at 1280x720. The reply has `renderer`, `width` and `height`. `emulated` is an error with no emulated GPU (`renderer` native) and while it skips the game's draws (`both` under `skip_draws` or `swap_only`; `capture` takes one it drew whole) |
| `capture [name] [composed]` | a screenshot (`<name>.png`), the native view's capture of the same frame (`<name>.cap`) and the native view's GPU drawing of it (`<name>.gpu.png`), under `screenshots/`. With `composed` it fails unless the capture is a post frame composed with the world frame before it; the files are written either way. Its reply, and how `renderer` native changes it, are under [Render checks](native-renderer.md#render-checks) |
| `native_view on [<width>x<height>] [nopost]\|off\|stats` | the native view live, as F9 draws it but without its window, at 1280x720 unless given a size, to see what it costs; `nopost` leaves out RB3's post-processing. While the native picture shows it draws at the window's size, so a size is an error. `off` stops it and replies with the run it ends. Every reply has `stats` ([below](#native_view-stats)) |
| `present_stats [reset]` | the window's pacing since `reset`, which replies with the stretch it ends ([below](#present_stats)) |
| `set <setting> <value>` | Band3 settings only |
| `cvar <setting>` | any setting's value, the SDK's too, and its `source`: `default`, `config` (`band3.toml`), `environment`, `command_line` or `runtime` (changed since startup); a password (`ha_mqtt_password`) reads `(hidden)` when it's set |
| `folders` | the folders the game runs with: `game_data`, `user_data`, `cache`, and `content` (`content_folders` as the song scan reads them) |
| `video_find <shortname>...` | finds and fetches these songs' music videos, a song at a time on the Videos tab's queue as its batch does (yt-dlp from `music_video_ytdlp_path`, into the first `music_videos_folder`); the reply's `queued` says how many (a song whose video was chosen by hand, or already queued, isn't). The log says how each went (`Music videos: <shortname>: ...`) |
| `bind <name>` | presses the key a key bind is set to, as if the window had focus: `instrument_lab` (F6), `pause_menu` (Escape), `native_view` (F9), `renderer` (F8), `liveless_rooms` (F10); the `bind_` prefix is optional. With `window offscreen`, `window shot` then captures the overlay it opens |
| `mouse move <x> <y>`, `mouse click [left\|right]`, `mouse wheel <notches>` | the window's mouse as if it had focus, in the window's pixels: the pointer moved there, a button pressed and let go where it is, the wheel turned (up positive). The window has to be restored (`window offscreen`). For [the mouse in menus](instruments.md#mouse-in-menus) |
| `mouse rows`, `mouse hover <display>`, `mouse target <n>` | what pointing sees, next frame: the focused list's rows (`display`, the place drawn from the top; `showing`; `data`; their anchors; `pickable`; `highlighted`) and bounds, and the components the pointer can move the focus to (`targets`, top to bottom, with their boxes), each as address and class, with the picture's place in the window. `hover` moves the pointer to the middle of the row drawn at that place, `target` to the middle of the nth target (from 0) |
| `type <text\|{key}>...` | types on the window's keyboard as if it had focus, as SDL delivers keys: each key goes down, then its character if the window was already taking text, then up. Text is letters, digits and `` - . , ' / ` ``; `{enter}` `{back}` `{tab}` `{esc}` `{space}` `{left}` `{right}` `{up}` `{down}` `{delete}` press one key each, as a token of their own: `type the {space} who {enter}`. For [typing to search songs](instruments.md#typing-to-search-songs) |
| `liveless_invite <host[:port]> [force_flag]` | player 1 accepts an invite to the [Liveless](integrations.md#liveless-online-play) game at that address (port 9103 unless given), as from the console's guide, and joins its band; it becomes `liveless_connect` from then on. Needs `liveless` on and the game online. `force_flag` also sets RB3's "joined through an invite" flag first, to tell whether a host turning the join away (error 10) is for want of it |
| `rooms_status [<field>=<value>\|<field>!=<value>\|<field>~<text>]...` | the Liveless Rooms client's status, in `rooms`: `state`, `server`, `code`, `public_ip` (as the server saw it), `advertised_ip` (what the game last told joining players), `error`, `last_join_user` and `last_join_ip`, `game_socket` (the game is online; joins need it), `retry_in` (seconds until it reconnects by itself; 0 when it won't) and `attempt` (from 1). With checks it fails unless each field is, isn't (`!=`) or contains (`~`) the value: `rooms_status state=logged_in code=HOST0001`, `rooms_status state=disconnected retry_in!=0` |
| `rooms_join <code>` | asks the Rooms server for the game with that code, as the panel's Join does, and replies without waiting for the answer: the game then gets an invite, or `rooms_status`'s `error` says why not. Needs `rooms=logged_in` and the game online |
| `rooms_connect` | connects to the Rooms server again now, without waiting for the client's own retry |
| `lights` | the [Stage Kits](integrations.md#stage-kit-lights) found, in `lights`: `devices` (each with its `key`, `kind`, `name`, `detail` and `online`, as the Lights tab lists them), `fake`, the `[left, right]` commands the pretend kit (launch with `-- --stagekit_fake=true`) has been sent since the last `lights`, and `problem`, why Picos can't be found, if they can't |
| `lights_test <left> <right> [<device>]` | a Stage Kit command (each byte decimal or `0x` hex) for the device a `lights` key names (`usb:fake`, `pico:127.0.0.1`), or every device, as the Lights tab's test controls send it |
| `port_mapping_status [<field>=<value>\|<field>!=<value>\|<field>~<text>]...` | Liveless' [port mapping](integrations.md#port-mapping), in `port_mapping`: `state`, `method` (`pcp`, `natpmp`, `upnp`), `external_ip`, `port`, `lease_s` (0 for a permanent UPnP mapping) and `error`. Checks as `rooms_status`'s: `port_mapping_status state=mapped method=pcp` |
| `quit` | |

| `window` subcommand | |
|---|---|
| `offscreen`, `onscreen` | as above |
| `shot <png>` | the client area as the window presents it, the SDK's overlays included (`screenshot` is the game's picture alone) |
| `size <W>x<H>` | the client area in physical pixels, kept no bigger than the monitors (`clamped` in the reply) |
| `minimize` | minimizes it again |
| `click <X>,<Y>` | clicks there (client pixels) with posted messages, without focus or the real cursor, for the launcher's buttons (`offscreen` first) |
| `status` | where it is |

### native_view stats

| Field | |
|---|---|
| `backend`, `captured`, `rendered`, `skipped_busy` | the backend, and since `on` the frames captured, drawn, and captured while it was still drawing another |
| `worldless` | frames drawn without a world under even/odd rendering; 0 unless that's broken |
| `ms`, `wait_ms` | each drawn frame's time (`mean`, `p50`, `p95`, `max`; on the GPU, uploads and reading back included), and the part after submitting. While the native picture shows on the zero-copy path, `ms` ends at submitting and `wait_ms` is the GPU time recording the next frame didn't hide |
| `in_flight_max` | the most of its frames the GPU had at once: 1, or 2 with `native_present_pipeline` |
| `paused`, `paused_ms`, `paused_captures` | with `renderer` native, the pause while the window is minimized |
| `game_frames`, `game_fps` | the game's own, since the last `on` or `off` |
| `rt_recording` | whether `native_view_record_targets` is on, and the texture passes the game drew while capture was off: how many were recorded, their draws, and the game thread's `ms` recording them |
| `capture` | what capturing cost the game's thread per game frame: `ms_per_frame` by hook, `us_per_draw`, counts `per_frame` (bytes copied and decoded, textures kept compressed or as R8) and the caches' `sizes`; with `native_view_capture_profile` on, `steps_ms_per_frame`. [Every field](dev/native-renderer-internals.md#native_view-stats-fields-in-full) |
| `emulated_gpu` | `mode` (`emulated_gpu_while_native`'s while the native picture shows, `full` otherwise), `skip_mode`, `skipping`, `fresh` (its picture is the game's), `frames` and `frames_skipped`; the calls `emitted_per_frame`, `skipped_per_frame` and `kept_per_frame` by emitter (`begin_indexed`, `indexed`, `instanced`, `up`, `clear`, `resolve`); `passes_dropped` under `swap_only`; `cp_ms_per_frame`, its command processor thread's CPU (-1 off Windows). With `renderer` native, `present` false and `sync` |
| `sync` | the sync-only GPU per game frame: `packets_per_frame`, `opcodes_per_frame` (the eight most sent), `draws_skipped_per_frame`, `waits_per_frame`, `stalled_waits`, `wait_ms_per_frame`, `wait_max_ms`, `stalled_by_interval` and `wait_intervals` (as the log's [summary](native-renderer.md#renderer-native-no-emulated-gpu)), `interrupts`, `swaps`, `vblanks`, `fences_per_frame`, `zpd_writes`, its threads' CPU (`sync_ms_per_frame`, `vblank_ms_per_frame`), and `unknown_opcodes`, `unknown_registers`, `bad_packets`, `bad_addresses` (0 expected) |
| `camera` | the frames drawn at a camera cut (`cuts`), where RB3's motion blur's shot frame count started over (`vel_resets`), and the cuts such a reset followed (`cuts_confirmed`) |
| `by_kind` | the same frames by what they drew under even/odd rendering (`world`, `post`, `between`, `full`); `{}` while it's off. Each kind has the worker's time by part (`parts_ms_per_frame`), what it drew, made and let go of (`per_frame`), the slowest parts (`plan_max_ms`), planning spikes, and with `native_gpu_timestamps` the GPU's time by part (`gpu_ms`). [Every field](dev/native-renderer-internals.md#native_view-stats-fields-in-full) |

`run` prints replies with `stats`. `tests/game/render_song_live.b3t` measures a song with it.

### present_stats

| Field | |
|---|---|
| `renderer`, `path` | `emulated` or `native`; `zero-copy` or `upload` (empty while emulated) |
| `paints`, `paint_ms`, `hitches` | the window's paints under either renderer, the time between them (`mean`, `p50`, `p95`, `max`), and intervals over 1.5 times the median. A paint's time is when the presenter asks for it, not when it reaches the screen |
| `shown`, `repeats`, `skipped`, `latency_ms` | under `native`: frames shown, paints showing the same frame as the one before, frames drawn that no paint showed, and the time from the game's Present to the first paint showing it |
| `step_ms`, `judder` | under `native`: for each paint after the first, how far the game's time moved on since the paint before (its frame's Present less the previous paint's frame's), and the steps where motion stood still or jumped: 0 (a repeat) or 1.5 times the median `paint_ms` or more. With the game at the display's rate it's steady at the game's interval when every refresh shows the next frame, and 0 and twice that by turns when the frame cap's beat and the vblanks drift in and out of phase |
| `published`, `publish_latency_ms` | the frames the native renderer handed the window, and the time from Present to handing it over, painted or not, so it measures off every monitor too |
| `game` | the game's `frames` and `fps` over the whole stretch, and its intervals (`ms`) and `hitches` over the last 8192 frames (about 136 s at 60 Hz) |
| `cap` | the frame cap's `mode` (`off`, `display`, `auto` or `fixed`) and `hz`, and in the same time the frames that ended `late` for their beat, the `resets` (a frame more than a beat late, which starts the beat again), and the mean a frame waited for its beat (`wait_ms`) and spun at the end of that wait (`spin_ms`) |
| `ui_round_trip` | how long the UI thread took to run something the game's joypad thread asked of it, after each time the game read a player (one request on its way at a time): `runs`, `runs_per_s`, and `p50`, `p95` and `max` in ms (a percentile to the quarter millisecond; over 64 ms it reads as `max`). The SDK's SDL input driver pumps SDL's events on the UI thread the same way, so an SDL pad's state is this old on top of its own polling; the other drivers (`input_backend` xinput, the HID instruments, the virtual instrument, MIDI) don't wait for it. Only under the test harness, which starts it |

A minimized window doesn't paint: `window offscreen` first. Off every monitor a window has no
real vertical blank, so its numbers compare renderers rather than measure what a monitor
would show. The same goes for `ui_round_trip`, which follows the window's paints: over
Remote Desktop the UI thread got round about 32 times a second with either renderer, the
round trips about 29 ms (p50), so measure it on the machine's own display.

## Players and online tests

Each player can have a virtual instrument: start a controller command with `p2`,
`p3` or `p4` for that player's (`p2 instrument drums`, `p2 hit red_pad`, `pad 2`);
without one it's player 1's. `tests/game/multiplayer.b3t` plays a two-player song, and the
render checks `render_multiplayer.b3t` and `render_multiplayer4.b3t` two- and four-part
ones. Players 2-4 aren't signed in, so they join as guests (`skip_profile_prompt`, on by
default).

Online tests never touch real services. Every launch with `liveless_rooms` on passes
`--liveless_rooms_server=127.0.0.1`, and under the harness band3 asks only
`liveless_gateway`/`liveless_upnp_url` for [port mapping](integrations.md#port-mapping) (without
them it maps nothing) and broadcasts no Pico discovery. Each script's or tool's header has
its launch lines.

| Test | Checks |
|---|---|
| `tests/game/liveless.b3t` | two band3s on this PC, one hosting [Liveless](integrations.md#liveless-online-play) and one joining. Each needs its own `--port`; the second its own `--user-data` and `liveless_port` |
| `tests/game/liveless_online.b3t` | takes one game online from the overshell. Run it on both, then `liveless_invite 127.0.0.1:9103` on the second joins the first's band |
| `tests/game/liveless_rooms.b3t` | a Rooms login against `tools/liveless_rooms_mock.py`: the code, staying logged in, the advertised address, a join by an unknown code. The mock's `--codes HOST0001,JOIN0001` hands out codes in connection order |
| `tests/game/liveless_rooms_reconnect.b3t` | reconnecting after the mock drops the game (`--drop-after-login 3 --drop-count 1`): `disconnected`, `retry_in` counting down, then `attempt` 2 |
| `liveless_rooms_host.b3t`, `liveless_rooms_join.b3t` | a join by code through a song on autoplay, the band's score matching on both. `python tools/liveless_rooms_play.py` runs it all, mock included |
| `tests/game/liveless_port_mapping.b3t` | mapping through `tools/port_mapping_mock.py` (PCP, `--mode natpmp`, or UPnP only with `--mode silent`) and the advertised address. `python tools/port_mapping_check.py` runs every case, checking each quit deletes the mapping |
| `python tools/test_home_assistant.py` | the [Home Assistant](integrations.md#home-assistant) link against `tools/ha_fake_broker.py`: discovery, the song, paused, progress and score topics, the webhook's payloads and the `offline` will |
| `python tools/test_stagekit_lights.py` | the [Stage Kit lights](integrations.md#stage-kit-lights) with the pretend USB kit (`stagekit_fake`) and `tools/fake_pico.py`: listing, test commands, a song's lighting reaching both, all-off after the song and at quit |
| `tests/game/usb_mic.b3t` | singing through the mics' test tone (`-- --usb_mics=true --usb_mic_test_tone=220`) raises the score |

The mocks have their own checks: `tools/test_liveless_rooms_mock.py`,
`tools/test_port_mapping_mock.py` and `tools/test_ha_fake_broker.py`.

## MIDI keyboards

With `midi_keys` on, `midi` plays the MIDI keyboard driver as if a keyboard sent it. With
`midi_keys_test_device` on too (read only by the harness), the driver reports a keyboard on
the port `harness` with no MIDI port open, so the test runs on any PC.

- The keyboard connects as player 2 and joins with its Start. It can't take player 1: RB3
  reads a player's type only when the slot connects, and player 1's never empties.
- `tests/game/midi_keys.b3t` works the menus, plays Pro Keys, pauses with the chord and plays
  5-lane Keys; its header has the launch line.

With `midi_keys_base_note` at 48, the keytar's 25 keys are 48-72 (`30`-`48`), and the menu
keys:

| Note | C 48 | D 50 | E 52 | F 53 | F# 54 | G 55 | A 57 | B 59 |
|---|---|---|---|---|---|---|---|---|
| Hex | `30` | `32` | `34` | `35` | `36` | `37` | `39` | `3b` |
| Button | Left | Down | Up | Right | Back | A | B | Start |

## Music videos

`tests/game/music_video.b3t` plays 20th Century Boy in a video venue with a music video for
it ([Music videos](settings.md#music-videos)): ffmpeg's test card, whose counter shows the
second it's at, started 2 s in by its `.ini`. Its header has the lines that make the video
and the launch line; the screenshots' counter reads the song's time plus 2, paused or not,
and the card's circle stays round. `tests/game/music_video_pattern.b3t` needs no video: with
`music_video_test_pattern` on, the venue's background is a pattern whose border, moving bar
and colour quadrants show the movie's planes are band3's, in both renderers.

`tests/game/music_video_hide_band.b3t` plays the song with `music_video_hide_band` on and the
pretend Stage Kit: only the video behind the highway, and `lights` still listing the venue's
commands. `tests/game/black_background_lights.b3t` turns on Deluxe's Black Background
modifier from the pause menu with `black_background_lights` on and plays the song again:
black behind the highway, and the kit still lit. Their headers have the launch lines.

## The main menu's Quit

`tests/game/quit_button.b3t` opens the main menu's Quit, backs out with Keep Rockin',
checks the button hides in Play Now's choices, and quits; the log then ends with "Game
requested exit". Start it on the main menu with no menu open: `launch` with a profile past
the first-run prompts, `wait screen=splash_screen timeout=120s`, `press start until
screen=main_hub_screen timeout=60s`, then `press red` in case Start opened the player's
menu (it does nothing on the main menu). Beside another band3, give it its own `--port`
and a copy of `out/test_user_data` as its `--user-data`.

On Rock Band 3 Deluxe the button is Deluxe's own Exit Game (made when its scripts see
`MHX_PC`, which band3 defines). band3's own Quit (`src/Hooks/quit_button.cpp`) appears only on
the game as it shipped. To test it, either:

- build from TU5's `default.xex` with TU5's `gen`, main and patch ARKs
  ([Building](building.md#game-files)); the disc's main ARK alone crashes in
  `Splash::PrepareNext`; or
- on Deluxe, build without the `-define MHX_PC` in `AddSettingArgs` (`src/config.cpp`).

## Inputs and timing

| Instrument | Inputs |
|---|---|
| all | `a b x y start back up down left right` |
| guitar | `green red yellow blue orange strum_up strum_down solo` |
| drums | `red_pad yellow_pad blue_pad green_pad yellow_cym blue_cym green_cym kick kick2` |
| keys | `overdrive` |

- Presses during a screen transition are dropped: wait for the screen you act on, `sleep` for
  a dialog to appear, and press with `until` where the press changes the screen.
- Submenus inside one screen (Play Now, Quickplay) don't change the screen name.
- A song counts as `in_game` until you leave its results; `state` keeps its details after.
- Switching `instrument` reconnects it, so press Start to join again.

## Overdrive

band3's `autoplay` never deploys overdrive: an autoplaying player ignores the deploy
buttons and the tilt. Rock Band 3 Deluxe's own Autoplay modifier does (its bots deploy
once their meters are half full), and `python tools/overdrive_check.py` turns it on
through the pause menu, plays a song, captures the moments after the first deploy and
sets the native renderer's picture of each against the emulated GPU's, for the
overdrive flames the native renderer once streaked across the venue. Its docstring has
the launch line and how to load a track theme into the test profile.

## Live pages

`tests/game/karaoke.b3t` plays 20th Century Boy on vocals (the USB mic's test tone singing)
with the web server on, and `tools/karaoke_check.py` then checks `/live/events` and
`/lyrics` against it: the state, the song clock's pace, the lyrics' shape, and the line
the karaoke page shows, printed to hold against a screenshot. The script's header has
the launch line.

## GPU hangs

`launch` starts band3 with `--dred=true` (Direct3D 12's Device Removed Extended Data;
`--dred=false` leaves it off). When the GPU hangs (`DEVICE_HUNG` in the log), band3 aborts
and its crash report, `logs/crash-<start>-<pid>.txt` beside band3.exe (the log names it),
gets the command lists the GPU hadn't finished and the op each stopped at; for
an indexed draw in the native renderer's last frame, also its mesh, target, counts, blend
and textures. Some hangs take the whole PC down instead, so nothing is written.

With `--d3d12_debug=true` the Direct3D 12 debug layer checks every call (Windows' optional
Graphics Tools feature, which has to be installed). Its errors end band3
(`d3d12_break_on_error`), and the crash trace gets each error's message with the stack of
the call: `band3.exe` frames are band3 or SDL_gpu, which `out/build/<preset>/band3.map`
resolves; `rexgpu-xenos.dll` frames are the SDK's emulated GPU.

`python tools/hang_bisect.py` boots band3 and plays `tests/game/pacing_song.b3t` at
refresh_rate 120 under settings that each take a renderer interaction away, counting the
runs that hung, in a journal that survives a hang taking the PC down; its docstring has
the settings and options.

`tests/game/pacing_song.b3t` plays 20th Century Boy and measures the native renderer's
pacing over 20 s of it (`native_view stats` and `present_stats`). Its header has the
launch line; `window offscreen` first, since a minimized window doesn't paint.
