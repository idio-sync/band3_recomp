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

`launch` starts the game minimized without taking focus (`--show` to watch it) and muted
(`--sound` to hear it), with its own saves in `out/test_user_data`, so tests never touch
your profile. A `.b3t` script is these commands one per line, with `#` comments; `run`
stops at the first one that fails, saves a screenshot of the moment and exits 1. Commands
joined with `;` share one connection, which `hold` needs: band3 lets go of everything held
when a client disconnects.

`test_port` also keeps the launcher away unless `--launcher` asks for it: `launch --
--launcher` shows it, and `launch` waits for the window, since nothing answers on the port
until the launcher's Play starts the game; the window commands below drive it. `launch
--no-harness -- --launcher` shows it without `test_port` at all.

`launch --config <file>` starts band3 with that file as the `band3.toml` beside the exe,
and puts back what was there (moved aside to `band3.toml.band3ctl`) once the harness
answers. It refuses `--no-harness` and `--launcher`, since the launcher would then save
into the player's own file; put the file beside the exe by hand for that. The command
line's `--user_data_root` wins over the file's. `python tools/test_config_file.py` uses it
to check that a `band3.toml` from the launcher's writer reaches the game, each value
winning over a `band3_config.ini` that sets it otherwise. It needs the unit tests built
(for `band3_write_config`), and runs band3 from a temporary folder.

The game starts on its default renderer: on Windows the [native renderer](native-renderer.md)
alone, so `screenshot` and `capture` take its picture and `screenshot emulated` is an error.
Add `-- --renderer=emulated` to `launch` for the emulated GPU alone, as the render scripts
do, or `-- --renderer=both` for the two side by side, where `bind renderer` (F8) switches
the picture and `screenshot`'s reply says which it took. `set renderer` switches between
`emulated` and `both` at once; to or from `native` it applies at the next launch
([choosing the renderer](native-renderer.md#choosing-the-renderer)).

`window` works on the game's window itself rather than through the harness, and never
activates it. A window launched minimized never paints, so `window offscreen` restores it to
the right of every monitor, where it paints unseen. `window onscreen` restores or moves it
to the top left of the primary monitor, unfocused, for pacing that a monitor's vertical
blank drives (`present_stats` can't measure that off every monitor). **It puts the game on
the user's screen**: ask first on a machine someone is using, and `window offscreen` or
`window minimize` it once done.

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms] [until <condition> [every=2s] [timeout=30s]]` | press and let go (100 ms); join inputs with `+`. With `until`, waits for the condition (as `wait` does) and presses again every `every` while the screen hasn't changed, for a press RB3 dropped |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `midi <hex bytes> [until <condition> [every=2s] [timeout=30s]]` | hands the [MIDI keyboard](#midi-keyboards) driver messages as its port would: `midi 90 3c 64` is a note-on. Each message starts with its status byte, so one command can hold several: `midi 90 30 64 80 30 00` taps the lowest C (a press is held at least 30 ms, a menu button 100 ms). With `until`, sends them all again every `every` while the screen hasn't changed, as `press` does. Fails unless `midi_keys` is on |
| `state` | the screen, whether a song is `paused`, song, venue, band, frame count, each player's instrument, the band's `score` (0 when a song starts), `ha_state` (the [Home Assistant](integrations.md#connection-state) connection, left out while it's `off`) and, while `usb_mics` records, the `mics` slots: what records each, whether the game connected it and the bytes `fed` to it |
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

`window` subcommands: `shot <png>` saves the client area as the window presents it, the
SDK's overlays included (`screenshot` is the game's picture alone); `size <W>x<H>` sets the
client area in physical pixels, kept no bigger than the monitors (`clamped` in the reply);
`minimize` minimizes it again, to restore onto the primary monitor; `click <X>,<Y>` clicks
there (client pixels) with posted messages, without focus or the real cursor, for the
launcher's buttons (it has to be `offscreen` to see them); `status` prints where it is.

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
| `capture` | what capturing cost the game's thread per game frame: `ms_per_frame` by hook (`mesh`, `multimesh`, `particles`, `rect`, `pass`, `present`, `other`), `us_per_draw`, counts `per_frame`, the caches' `sizes`, and with `native_view_capture_profile` on `steps_ms_per_frame`. Of the counts, `geom_copy_bytes` and `tex_copy_bytes` are the meshes' buffers and the textures' levels the game's thread copied out of guest memory where it saw them first or changed, to be decoded off it (`src/Render/deferred_decode.h`), `tex_decode_bytes` the RGBA it decoded itself (a render target's guest pixels, the noise map), and `deferred_decodes`, `deferred_decode_us` and `deferred_decode_bytes` (decoded Vertex, index and RGBA bytes) what the copies cost whoever took a capture with them first, the native renderer's worker or a `capture`: not the game's thread; of those, `deferred_bc_blocks` the block-compressed textures kept as blocks for the GPU (`native_bc_textures`; their bytes the blocks'), `deferred_bc_swizzled` those decoded to RGBA for a swizzle, and `deferred_bc_rgba` those kept as blocks whose RGBA something asked for after. `by_kind`'s `capture` has the game thread's three by kind of frame |
| `emulated_gpu` | `mode` (`emulated_gpu_while_native`'s while the native picture shows, `full` otherwise), `skip_mode`, `skipping`, `fresh` (its picture is the game's), `frames` and `frames_skipped`; the calls `emitted_per_frame`, `skipped_per_frame` and `kept_per_frame` by emitter (`begin_indexed`, `indexed`, `instanced`, `up`, `clear`, `resolve`); `passes_dropped` under `swap_only`; `cp_ms_per_frame`, its command processor thread's CPU (-1 off Windows). With `renderer` native, `present` false and `sync` |
| `sync` | the sync-only GPU per game frame: `packets_per_frame`, `opcodes_per_frame` (the eight most sent), `draws_skipped_per_frame`, `waits_per_frame`, `stalled_waits`, `wait_ms_per_frame`, `wait_max_ms`, `stalled_by_interval` and `wait_intervals` (as the log's [summary](native-renderer.md#renderer-native-no-emulated-gpu)), `interrupts`, `swaps`, `vblanks`, `fences_per_frame`, `zpd_writes`, its threads' CPU (`sync_ms_per_frame`, `vblank_ms_per_frame`), and `unknown_opcodes`, `unknown_registers`, `bad_packets`, `bad_addresses` (0 expected) |
| `camera` | the frames drawn at a camera cut (`cuts`), where RB3's motion blur's shot frame count started over (`vel_resets`), and the cuts such a reset followed (`cuts_confirmed`) |
| `by_kind` | the same frames by what they drew under even/odd rendering ([native renderer](native-renderer.md)); `{}` while it's off. Each kind's `parts_ms_per_frame` has `decode` (the worker decoding what capture left it to, at first sight of a song's or a shot's content; in `ms` too) and planning's parts too (`plan_setup`, `plan_walk`, `plan_targets`, `plan_arrays`, `plan_arena`, `plan_reserve`, and record's `post_plan`), which don't add up to the total; `per_frame` has the render targets made (`targets_made`, `targets_returning`), forgotten (`evicted_rts`) and released (`rts_released`), the textures and meshes drawn for the first time, the arrays grown, an arena rebuild's meshes copied on the GPU (`arena_copied`), what was let go early for room (`textures_pressured`, `meshes_pressured`) and kept by the 30 s alone (`meshes_by_time`, `textures_by_time`, in a song), and `rts_mb`; `plan_max_ms` the most each part took; `plan_spikes` the frames whose planning took over 8 ms, every one, and `plan_spikes_at_cut` those at a camera cut or up to 2 game frames after; with `native_gpu_timestamps` (Direct3D 12) also `gpu_timed` (the frames with GPU timings), `gpu_ms` (the GPU's milliseconds by part, per frame timed; `idle` is the GPU waiting for the CPU between command buffers), `gpu_total_ms` (each frame's busy total, all but `idle`) and `gpu_marks_dropped`/`gpu_bad_spans` (0 when all went well) |

`run` prints replies with `stats`. `tests/game/render_song_live.b3t` measures a song with it.

### present_stats

| Field | |
|---|---|
| `renderer`, `path` | `emulated` or `native`; `zero-copy` or `upload` (empty while emulated) |
| `paints`, `paint_ms`, `hitches` | the window's paints under either renderer, the time between them (`mean`, `p50`, `p95`, `max`), and intervals over 1.5 times the median. A paint's time is when the presenter asks for it, not when it reaches the screen |
| `shown`, `repeats`, `skipped`, `latency_ms` | under `native`: frames shown, paints showing the same frame as the one before, frames drawn that no paint showed, and the time from the game's Present to the first paint showing it |
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

`tests/game/liveless.b3t` takes two band3s on this PC online with Liveless, one hosting
and one joining it (its header has the two launches: each needs its own `--port`, and the
second its own `--user-data` and `liveless_port`). `tests/game/liveless_online.b3t` only
takes one online, from the overshell: run it on both, then `liveless_invite
127.0.0.1:9103` on the second joins the first's band. The host then goes Play Now →
Quickplay → Choose Songs, and waits until the joiner picks the same.

`tests/game/liveless_rooms.b3t` logs a game in to a Liveless Rooms server and takes it
online, checking its code, that it stays logged in through the server's pings, the
address it tells joining players and a join by a code no game has. It runs against
`tools/liveless_rooms_mock.py`, a stand-in server on this PC (its header has the command
lines): every test launch with `liveless_rooms` on passes
`--liveless_rooms_server=127.0.0.1`, never the public server. The mock's `--codes
HOST0001,JOIN0001` hands out those codes in connection order, so with two games online
`rooms_join HOST0001` on the second joins the first's band. `python
tools/test_liveless_rooms_mock.py` checks the mock itself.

`tests/game/liveless_rooms_reconnect.b3t` checks that a game reconnects by itself when
the server drops it (the mock's `--drop-after-login 3 --drop-count 1`): the state goes
`disconnected` with `retry_in` counting down, and the game logs in again with the next
code (`attempt` 2).

`tests/game/liveless_rooms_host.b3t` and `liveless_rooms_join.b3t` take that join through
a song: the joiner joins by code, each waits for the other, and both play 20th Century Boy
on autoplay to the results, where `state`'s score is the band's on both (the host's
header has the launches and the order). `python
tools/liveless_rooms_play.py` runs all of it, mock included, and fails unless the two band
scores match.

Under the harness band3 never asks the real router for Liveless'
[port mapping](integrations.md#port-mapping): PCP and NAT-PMP only go to
`liveless_gateway` and UPnP only to `liveless_upnp_url`, and without either it maps
nothing (`error` "skipped under the test harness"). Both point at
`tools/port_mapping_mock.py`, a stand-in router on 127.0.0.1 that answers PCP, NAT-PMP
(`--mode natpmp`) or only UPnP (`--mode silent`). `tests/game/liveless_port_mapping.b3t`
maps the port through it and checks the address the game advertises, and `python
tools/port_mapping_check.py` runs every way (each protocol, permanent UPnP mappings, an
old band3 mapping (`--upnp-error 718`) or another PC's, the harness guard,
`liveless_external_ip` winning), checking each quit deletes the mapping. `python
tools/test_port_mapping_mock.py` checks the mock itself.

`python tools/test_home_assistant.py` checks the [Home Assistant](integrations.md) link
against `tools/ha_fake_broker.py`, a fake MQTT broker and webhook receiver on 127.0.0.1
(`python tools/ha_fake_broker.py --mqtt-port 1883 --webhook-port 8124` runs it alone,
printing what arrives). It launches band3 pointed at it, plays 20th Century Boy on
autoplay, pauses and leaves it, then kills band3: the discovery configs, the song,
playing, paused, progress and score topics, the webhook's payloads and the will's
`offline` on the status topic are all checked. `python tools/test_ha_fake_broker.py`
checks the fake itself, offline.

`python tools/test_stagekit_lights.py` checks the [Stage Kit lights](integrations.md#stage-kit-lights)
with the pretend USB kit (`stagekit_fake`) and `tools/fake_pico.py`, a stand-in Pico W
on 127.0.0.1 that sends its telemetry unasked: both listed, test commands reaching the
device named and no other, 20th Century Boy's lighting reaching both (the Pico's through
the RB3Enhanced events), the lights off after the song, and the Pico sent all-off when
band3 quits. Under the harness band3 broadcasts no Pico discovery, so the network's
Picos are left alone.

`tests/game/usb_mic.b3t` sings a song's vocals through the USB mics' test tone (launch
with `-- --usb_mics=true --usb_mic_test_tone=220`) and waits for the score to go up.

## MIDI keyboards

With `midi_keys` on, `midi` plays the MIDI keyboard driver as if a keyboard sent it. With
`midi_keys_test_device` on too (a setting only the harness reads, kept out of the launcher
and the in-game settings), the driver reports a keyboard on the port `harness` from the
first message `midi` hands it, with no MIDI port open, so the test runs on any PC. It
connects as player 2, beside player 1's virtual instrument, and joins with its Start:
RB3 reads a player's type only when the player's slot connects, and player 1's never
empties (once the virtual instrument goes, the SDK's stand-in device holds it), so a
keyboard swapped in there would go on reading as the old instrument.
`tests/game/midi_keys.b3t` works the menus from the keyboard, plays Pro Keys, pauses with
the chord and plays 5-lane Keys that way; its header has the launch line. With
`midi_keys_base_note` at 48, the
menu keys are C 48 (`30`) Left, D 50 (`32`) Down, E 52 (`34`) Up, F 53 (`35`) Right, F#
54 (`36`) Back, G 55 (`37`) A, A 57 (`39`) B and B 59 (`3b`) Start, and the keytar's 25
keys are 48-72 (`30`-`48`).

## The main menu's Quit

`tests/game/quit_button.b3t` opens the main menu's Quit, backs out with Keep Rockin',
checks the button hides in Play Now's choices, and quits; the log then ends with "Game
requested exit". Start it on the main menu with no menu open: `launch` with a profile past
the first-run prompts, `wait screen=splash_screen timeout=120s`, `press start until
screen=main_hub_screen timeout=60s`, then `press red` in case Start opened the player's
menu (it does nothing on the main menu). Beside another band3, give it its own `--port`
and a copy of `out/test_user_data` as its `--user-data`.

On Rock Band 3 Deluxe the button is Deluxe's own Exit Game (its scripts make it when they
see `MHX_PC`, which band3 defines); band3 makes its own Quit
(`src/Hooks/quit_button.cpp`) only on the game as it shipped. Testing that one needs band3
built from TU5's `default.xex` with TU5's `gen`, main and patch ARKs both
([Building](building.md#game-files)). A folder with only the disc's main ARK won't do: a
build from Deluxe's `default.xex` crashes there as it starts, in `Splash::PrepareNext`. On
Deluxe, a build that leaves the `-define MHX_PC` out of `AddSettingArgs` (`src/config.cpp`)
shows band3's Quit instead, on the same main menu and hint screens the game shipped with,
since Deluxe keeps those.

## Inputs and timing

Every instrument has `a b x y start back up down left right`; guitars add `green red
yellow blue orange strum_up strum_down solo`, drums `red_pad yellow_pad blue_pad
green_pad yellow_cym blue_cym green_cym kick kick2`, keys `overdrive`. Presses during a
screen transition are dropped, so wait for the screen you act on (and give a dialog a
moment to appear, with `sleep`), and press with `until` where the press changes the
screen. Submenus inside one screen (Play Now, Quickplay) don't change the screen name. A
song counts as `in_game` until you leave its results, and `state` keeps the last song's
details after that. Switching `instrument` reconnects it, so press Start to join again.

## Overdrive

band3's `autoplay` never deploys overdrive: an autoplaying player ignores the deploy
buttons and the tilt. Rock Band 3 Deluxe's own Autoplay modifier does (its bots deploy
once their meters are half full), and `python tools/overdrive_check.py` turns it on
through the pause menu, plays a song, captures the moments after the first deploy and
sets the native renderer's picture of each against the emulated GPU's, for the
overdrive flames the native renderer once streaked across the venue. Its docstring has
the launch line and how to load a track theme into the test profile.

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
