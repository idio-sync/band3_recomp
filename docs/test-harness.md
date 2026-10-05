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
(`--sound` to hear it), and
gives it its own saves in `out/test_user_data`, so tests never touch your profile. A
`.b3t` script is these commands one per line, with `#` comments; `run` stops at the
first one that fails, saves a screenshot of the moment and exits 1. Commands joined
with `;` share one connection, which `hold` needs: band3 lets go of everything held
when a client disconnects.

`test_port` also keeps the launcher (band3's setup screen) away, unless `--launcher` asks
for it: `launch -- --launcher` shows the launcher under the harness. Nothing answers on the
port until the launcher's Play starts the game, so `launch` waits for the window instead;
the window commands below drive the launcher, and once Play is clicked the harness answers
as usual. `launch --no-harness -- --launcher` shows it without `test_port` at all.

`launch --config <file>` starts band3 with that file as the `band3.toml` beside the
exe, and puts back what was there (moved aside to `band3.toml.band3ctl` meanwhile) as soon
as the harness answers, by when band3 has read it. It refuses `--no-harness` and
`--launcher`: the launcher showing then saves to `band3.toml` after the file is put back,
into the player's own. Put
the file beside the exe by hand to start the launcher with it. `--user_data_root` is on
the command line, so the file's `user_data_root` loses to it. `tools/test_config_file.py`
uses it to check that a `band3.toml` written by the launcher's writer reaches the game: its
folders (absolute with forward slashes, and relative to the ini's folder),
`content_folders`, `controller_type` and `lang`, each winning over a `band3_config.ini`
that sets them otherwise, and a bool, a float and a string with a quote and a backslash,
read back as written. It needs the unit tests built, for `band3_write_config` (the writer
as a small program), and runs band3 from a temporary folder it deletes afterwards:

```
python tools/test_config_file.py
```

The game starts on its default renderer: on Windows the [native renderer](native-renderer.md),
so `screenshot` takes its picture (`renderer` `native` in the reply). Add `-- --renderer=emulated`
to `launch` for the emulated GPU's, as the render scripts' launch lines do. `-- --emulated_gpu=off`
runs without the emulated GPU at all ([experimental](native-renderer.md#running-without-the-emulated-gpu-emulated_gpu-off)):
the native renderer's picture is the only one, so `screenshot emulated` is an error and
`capture`'s screenshot is the native renderer's.

`capture` and `native_view` check the native view's rendering; see
[Render checks](native-renderer.md#render-checks).

`window` works on the game's window itself rather than through the harness, and never
activates it; only `onscreen` puts it on a monitor. A window launched minimized never
paints, so `window offscreen` restores it to the right of every monitor, where it paints
unseen. `window onscreen` restores it (or moves it) to the top left of the primary
monitor's work area, its size kept, without activating it or changing its place among the
other windows, for pacing that a monitor's vertical blank drives (off every monitor
`present_stats` can't measure that). **It puts the game on the user's screen**, where it
may cover what they're working on: ask first on a machine someone is using, and `window
offscreen` or `window minimize` it once done. `window
shot <png>` saves its client area as the window presents it, the SDK's overlays
included (`screenshot` is the game's picture alone); `window size <W>x<H>` sets
its client area in physical pixels, though Windows keeps it no bigger than the monitors
(`clamped` in the reply); `window minimize` minimizes it again, and moves where it
restores to back onto the primary monitor. `window click <X>,<Y>` clicks there (client
pixels) with posted mouse messages, without focus or the real cursor, for the launcher's
buttons; the window has to be `offscreen` to see it. `window status` prints where it is.

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms] [until <condition> [every=2s] [timeout=30s]]` | press and let go (100 ms); join inputs with `+`. With `until`, waits for the condition (as `wait` does) and presses again each time `every` goes by with the screen still the one it pressed on, for a press RB3 dropped; once the screen has changed at all, it only waits |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `state` | the screen, song, venue, band, frame count, each player's instrument, the band's `score` (the song's scoreboard, 0 when a song starts) and, while `usb_mics` records, the `mics` slots: what records each, whether the game connected it and the bytes `fed` to it |
| `pad [player]` | the buttons, triggers and sticks the game reads from a player (1-4) |
| `wait <condition> [timeout=30s]`, `expect <condition> [timeout=5s]` | `screen=`, `screen~` (contains), `in_game`, `menus`, `song=<shortname>`, `frames=<n>`, `score>=<n>`, `mic=<slot>` (connected and fed audio since the wait began), `rooms=<off\|connecting\|connected\|logged_in\|disconnected\|failed>` (the Liveless Rooms connection's state, which `state` shows as `rooms` once it isn't `off`), `port_mapping=<off\|searching\|mapped\|failed>` (Liveless' port mapping on the router, which `state` shows as `port_mapping` once it isn't `off`), `joined` (an online band formed with this game in it, since it started: as the host, it let a player join; joining, the host said yes. `state` shows `"joined":true` from then on) |
| `sleep <n>s\|<n>ms` | waits that long, up to 600 s: `frames=` counts the main thread's frames, which stand still while the boot logos play on RB3's splash thread, and pass twice as fast at `refresh_rate` 120, so a pause for a menu to settle is a `sleep` |
| `screenshot [emulated\|native] [name]` | the picture the window shows: the emulated GPU's at the game's size, or with `renderer` native the [native renderer](native-renderer.md)'s next frame at the size it draws at (the window's picture's, minimized too). `emulated` or `native` takes that one whatever the window shows; `native` while the native renderer is off draws the game's next frame once at 1280x720. The reply has `renderer` (`emulated` or `native`), `width` and `height`. The emulated GPU's picture is an error while it skips the game's draws (`renderer` native, `emulated_gpu_while_native` `skip_draws` or `swap_only`): it isn't the game's then; `capture` takes one it drew whole (under `swap_only` still without what RB3 drew once meanwhile: `emulated_passes_dropped`). With `emulated_gpu` off there's no emulated GPU: the picture is the native renderer's, and `emulated` is an error, "no emulated GPU this run (emulated_gpu off)" |
| `capture [name] [composed]` | a screenshot (`<name>.png`) and the native view's capture (`<name>.cap`) of the same full frame, and the native view's GPU backend drawing it (`<name>.gpu.png`), all under `screenshots/`. With `composed` it fails unless the capture is a post frame composed with the world frame before it (`proc_cmds` 2, `composed` true); its files are written either way. The reply's fields are under [Render checks](native-renderer.md#render-checks). With `emulated_gpu` off it holds a frame without drawing whole frames first, `<name>.png` is the native renderer's picture at the window's picture's size, `<name>.gpu.png` is at 1280x720, and `emulated` is `none`. `run` prints its reply, for `tools/capdiff.py` to read from the run's output ([Render checks](native-renderer.md#render-checks)) |
| `native_view on [<width>x<height>] [nopost]\|off\|stats` | the native view live, as F9 draws it (same worker, same `native_view_backend`) but without its window, at 1280x720 unless given a size, and without RB3's post-processing with `nopost` (`post` in the reply), to see what it costs; `off` stops it and the capturing with it. While `renderer` is native the native renderer draws at the window's size, so `on` measures it at that size and `on` with a size is an error. Each reply has `stats`: the backend, frames `captured` and `rendered` since `on`, `skipped_busy` (captured while it was still drawing another), `worldless` (drawn without a world: with even/odd rendering, a frame that drew none and wasn't composed with the one before; 0 unless that's broken), each drawn frame's time in `ms` (`mean`, `p50`, `p95`, `max`; on the GPU the whole frame, uploads and reading back included) and the part after submitting in `wait_ms` (while `renderer` is native on the zero-copy path, `ms` is the frame up to submitting it alone, and `wait_ms` the time the worker then waited for the GPU to finish it before handing it to the window, the GPU's time that recording the next frame didn't hide), `in_flight_max` (the most of its frames the GPU had at once: 1 unless `native_present_pipeline` is on, then 2 when it recorded a frame while the GPU drew the one before, never more), with `emulated_gpu` off the native renderer's pause while the window is minimized (`paused`: now; `paused_ms` and `paused_captures`, the captures the game published while it drew nothing, neither `rendered` nor `skipped_busy`; false and 0 with the emulated GPU on), and the game's own `game_frames` and `game_fps` since the last `on` or `off`, and `rt_recording`: whether `native_view_record_targets` is `on`, and the texture passes the game drew in that time while capture was off, how many of them were recorded (those into textures that aren't drawn every frame or every world frame), their draws, and the game thread's `ms` recording them; and `emulated_gpu`: `emulated_gpu_while_native`'s `mode` while `renderer` is native (`full` otherwise) and whether it skips anything now (`skip_mode`), the frame being drawn is skipped (`skipping`) and its picture is the game's (`fresh`), the game's `frames` and `frames_skipped`, the calls per frame `emitted_per_frame` and `skipped_per_frame` by emitter (`begin_indexed`, never skipped; `indexed`, the meshes; `instanced`; `up`, quads; `clear` and `resolve`, skipped only by `swap_only`), of those emitted in `skip_draws` frames, those kept for a texture pass drawn once and the flares' occlusion tests (`kept_per_frame`), the passes drawn once whose draws `swap_only` skipped (`passes_dropped`, a count), and the CPU the emulated GPU's command processor thread used per game frame (`cp_ms_per_frame`, kernel and user; -1 off Windows or when the thread isn't found). With `emulated_gpu` off it also has `present` false (absent otherwise; `mode` is then `swap_only` and `fresh` false) and `sync`: what band3's sync-only GPU did in the same time, per game frame where it's busy: `packets_per_frame`, `opcodes_per_frame` (the eight most sent, by name), `draws_skipped_per_frame`, `waits_per_frame`, `stalled_waits` (waits that had to wait), `wait_ms_per_frame`, `wait_max_ms` (the longest since the game started), `stalled_by_interval` (the stalled waits by their packet's wait interval: `yield` under 0x100, which polls again at once, a spin; `sleep` 0x100 to 0xFFF; `long_sleep` 0x1000 and up; each with its `waits`, their `ms` and the `polls` that didn't match), `wait_intervals` (stalled waits by the interval itself, in hex, most first; the first 8 intervals seen), `interrupts`, `swaps`, `vblanks`, `fences_per_frame`, `zpd_writes` (occlusion query results), `unknown_opcodes`, `unknown_registers`, `bad_packets`, `bad_addresses` (0 expected for all four), and its command processor thread's and vblank thread's CPU per frame (`sync_ms_per_frame`, `vblank_ms_per_frame`; -1 off Windows); and `by_kind`: the same frames by what they drew under even/odd rendering (`world`, `post`, `between`, `full`), each kind's `rendered`, the `skipped_busy` charged to it (captured while one of its frames was being drawn), `ms` and `wait_ms`, and per frame drawn the worker's parts, what it sent, made and let go of, and what capturing it cost the game's thread (each listed in [the native renderer's](native-renderer.md) settings; `{}` while it's off). `off`'s reply is the run it ends; `run` prints replies with `stats`. `tests/game/render_song_live.b3t` measures a song with it |
| `present_stats [reset]` | the window's pacing since `reset` (which replies with the stretch it ends and starts over), in `stats`: `renderer` (`emulated` or `native`) and `path` (`zero-copy` or `upload`, empty while emulated); the window's `paints` under either renderer and the time between them, `paint_ms` (`mean`, `p50`, `p95`, `max`), and `hitches` (intervals over 1.5 times the median); under `native`, its `paints`, the native frames `shown`, `repeats` (a paint showing the frame the one before showed), `skipped` (frames drawn that no paint showed) and `latency_ms` from the game's Present of each frame shown to the first paint showing it, and the new frames the native renderer handed the window (`published`) with `publish_latency_ms` from the game's Present of each to its handing it over, painted or not (so it measures the renderer off every monitor too, and under either `emulated_gpu`); and the `game`'s own `frames` and `fps` in the same time and its intervals (`ms`) and `hitches` over the stretch's last 8192 frames (all of it up to about 136 s at 60 Hz; `frames` and `fps` always cover all of it), with the frame cap's `cap`: its `mode` now (`off`, `display`, `auto` or `fixed`) and rate (`hz`), and in the same time the frames that ended `late` for their beat (by less than a frame), the `resets` (a frame later than that, a hitch or a render check's hold, starting the beat again), and the mean a frame waited for its beat (`wait_ms`) and spun at the end of that wait (`spin_ms`). A paint's time is when the presenter asks for it, not when it reaches the screen. A minimized window doesn't paint: `window offscreen` first. Off every monitor a window has no real vertical blank, so its numbers compare renderers under the same conditions rather than measure what a monitor would show |
| `set <setting> <value>` | Band3 settings only |
| `cvar <setting>` | any setting's value, the SDK's too, and its `source`: `default`, `config` (`band3.toml`), `environment`, `command_line` or `runtime` (changed since startup, by F4, `set`, band3_config.ini or band3 itself) |
| `folders` | the folders the game runs with: `game_data`, `user_data`, `cache`, and `content`, the `content_folders` resolved as the song scan reads them |
| `bind <name>` | presses the key a key bind is set to, as if the window had focus: `bind instrument_lab` (F6), `bind settings` (F4), `bind native_view` (F9), `bind renderer` (F8), `bind liveless_rooms` (F10, the Rooms panel); the `bind_` prefix is optional. With `window offscreen`, `window shot` then captures the overlay it opens |
| `liveless_invite <host[:port]> [force_flag]` | player 1 accepts an invite to the [Liveless](integrations.md#liveless-online-play) game at that address (port 9103 unless given), as from the console's guide: the game is told of it (`XN_LIVE_INVITE_ACCEPTED`), reads it, and joins that game's band, which also becomes the game to join (`liveless_connect`) from then on. Needs `liveless` on and the game online (the overshell's Play on Xbox Live). `force_flag` also sets RB3's "joined through an invite" flag first, which the game's own invite handling sets anyway; it's there to tell whether a host turning the join away (error 10) is for want of it |
| `rooms_status [<field>=<value>\|<field>!=<value>\|<field>~<text>]...` | the Liveless Rooms client's status, in `rooms`: `state` (as `rooms=` names it), `server` (`liveless_rooms_server`), this player's `code`, `public_ip` (this PC as the server saw it), `advertised_ip` (the address the game last told players joining it), `error` (the last thing that went wrong), `last_join_user` and `last_join_ip` (the last game a join went to; addresses empty for none), `game_socket` (whether the game's online socket is open: it's gone online and not left again; joins and NAT punches need it), `retry_in` (seconds until the client connects again by itself after the connection went, rounded up; 0 when it won't) and `attempt` (which connection this is since the game started or `rooms_connect`, from 1). With checks it fails unless each field is the value, with `!=` isn't it, or with `~` has the text in it (for errors, which have spaces): `rooms_status state=logged_in code=HOST0001 error~NOPE0000`, `rooms_status state=disconnected retry_in!=0` |
| `rooms_join <code>` | asks the Rooms server for the game with that code (1-8 letters and digits, any case), as the Rooms panel's Join does; replies once it's asked, without waiting for the answer: the game then gets an invite to that game, or `rooms_status`'s `error` says why not. Needs `rooms=logged_in` and the game online (the overshell's Play on Xbox Live, `game_socket` true): before that it's an error, "the game isn't online yet" |
| `rooms_connect` | connects to the Rooms server again now, after it disconnected or failed, without waiting for the client's own retry |
| `port_mapping_status [<field>=<value>\|<field>!=<value>\|<field>~<text>]...` | Liveless' [port mapping](integrations.md#port-mapping) on the router, in `port_mapping`: `state` (as `port_mapping=` names it), `method` (`pcp`, `natpmp`, `upnp`, or empty before any), `external_ip` (the router's public address, empty until it says), `port` (`liveless_port`), `lease_s` (what the router granted; 0 for a permanent UPnP mapping, or not mapped) and `error` (why it isn't mapped). With checks it fails unless each field is the value, with `!=` isn't it, or with `~` has the text in it: `port_mapping_status state=mapped method=pcp external_ip=203.0.113.5`, `port_mapping_status state=mapped lease_s!=0` |
| `quit` | |

Each player can have a virtual instrument: start a controller command with `p2`,
`p3` or `p4` for that player's (`p2 instrument drums`, `p2 hit red_pad`, `pad 2`);
without one it's player 1's. `state` lists every player's instrument.
`tests/game/multiplayer.b3t` plays a two-player song, and the render checks
`render_multiplayer.b3t` and `render_multiplayer4.b3t` two- and four-part ones
([Render checks](native-renderer.md#render-checks)). Players 2-4 aren't signed in,
so they join as guests (`skip_profile_prompt`, on by default). With it off, RB3 asks
each to choose a profile when they join: No Profile plays as a guest.

`tests/game/liveless.b3t` takes two band3s on this PC online with Liveless, one
hosting and one joining it (its header has the two launches: each needs its own
`--port`, and the second its own `--user-data` and `liveless_port`).
`tests/game/liveless_online.b3t` only takes one online, from the overshell, without
searching: run it on both, then `liveless_invite 127.0.0.1:9103` on the second
joins the first's band by invite. The host then goes Play Now → Quickplay → Choose
Songs, and waits ("Waiting for Xbox LIVE Players...") until the joiner picks the same.

`tests/game/liveless_rooms.b3t` logs a game in to a Liveless Rooms server and takes it
online, checking its code, that it stays logged in through the server's pings, the
address it tells joining players and a join by a code no game has. It runs against
`tools/liveless_rooms_mock.py`, a stand-in server on this PC (its header has the mock's
and the game's command lines): every test launch with `liveless_rooms` on passes
`--liveless_rooms_server=127.0.0.1`, never the public server. The mock's `--codes
HOST0001,JOIN0001` hands out those codes in connection order, so with two games online
(`liveless_online.b3t` on both) `rooms_join HOST0001` on the second joins the first's
band. `python tools/test_liveless_rooms_mock.py` checks the mock itself.

`tests/game/liveless_rooms_reconnect.b3t` checks that a game connects again by itself
when the server drops it: the mock's `--drop-after-login 3 --drop-count 1` hangs up on
the first login 3 s in, the state goes `disconnected` with `retry_in` counting down,
and the game logs in again with the next code (`attempt` 2). Its header has the
launches.

`tests/game/liveless_rooms_host.b3t` and `tests/game/liveless_rooms_join.b3t` take that
join through a song: the joiner joins by code, each waits for the other (`wait joined`,
then the game's own "Waiting for Xbox LIVE Players"), and both play 20th Century Boy on
autoplay to the results, where `state`'s score is the band's on both. The host's header
has the launches and the order; `python tools/liveless_rooms_play.py` runs all of it,
mock included, and fails unless the two band scores match.

Liveless' [port mapping](integrations.md#port-mapping) talks to the router, so under the
harness (`test_port` set) band3 never asks the real one: PCP and NAT-PMP only go to
`liveless_gateway` and UPnP only to `liveless_upnp_url`, and without either it maps
nothing (`port_mapping_status`'s `error` "skipped under the test harness"). Both point
at `tools/port_mapping_mock.py`, a stand-in router on 127.0.0.1 that answers PCP,
NAT-PMP (`--mode natpmp`) or only UPnP (`--mode silent`), and logs each request and
delete. `tests/game/liveless_port_mapping.b3t` maps the port through it and checks the
address the game then advertises (its header has the launches), and `python
tools/port_mapping_check.py` runs every way (PCP, NAT-PMP, UPnP, a UPnP router that only
maps for good, one holding an old band3 mapping of the port (`--upnp-error 718`) or
another PC's, the harness guard, `liveless_external_ip` winning), mock included,
checking each quit deletes the mapping. `python tools/test_port_mapping_mock.py` checks
the mock itself.

`tests/game/usb_mic.b3t` sings a song's vocals through the USB mics' test tone (launch
with `-- --usb_mics=true --usb_mic_test_tone=220`) and waits for the score to go up.

Every instrument has `a b x y start back up down left right`; guitars add `green red
yellow blue orange strum_up strum_down solo`, drums `red_pad yellow_pad blue_pad
green_pad yellow_cym blue_cym green_cym kick kick2`, keys `overdrive`. Presses during
a screen transition are dropped, so wait for the screen you act on (and give a dialog
a moment to finish appearing, with `sleep`), and press with `until` where the press
changes the screen, so a dropped one is made again. Submenus inside one screen (Play Now, Quickplay) don't
change the screen name. A song counts as `in_game` until you leave its results, and
`state` keeps the last song's details after that. Switching `instrument` reconnects
it, so press Start to join again, as a player would.

## GPU hangs

`launch` starts band3 with `--dred=true` (`--dred=false` leaves it off): Direct3D 12's
Device Removed Extended Data. When the GPU hangs (`DEVICE_HUNG` in the log), band3
aborts and `band3_crash_trace.txt`, in the folder it runs from (the repository root
under the harness), gets the command lists the GPU hadn't finished and the op each
stopped at. A list stopped at an indexed draw says which of the list's indexed draws
it is, and, where it's one of the native renderer's last frame, what that draw was:
its mesh, its target, its counts, its blend and its textures. Some hangs take the
whole PC down instead, so nothing is written.

With `--d3d12_debug=true`, the Direct3D 12 debug layer checks every call (it's
Windows' optional Graphics Tools feature, which has to be installed). Its errors end
band3 (`d3d12_break_on_error`), and the crash trace gets each error's message with
the stack of the call it's about: `band3.exe` frames are band3 or SDL_gpu, which
`out/build/<preset>/band3.map` resolves; `rexgpu-xenos.dll` frames are the SDK's
emulated GPU.

`tools/hang_bisect.py` boots band3 and plays `tests/game/pacing_song.b3t` at
refresh_rate 120 under settings that each take a renderer interaction away
(`baseline`, `no-zero-copy`, `emulated-full`, `emulated`), a few rounds of each, and
counts the runs that hung. Each run goes to `out/hang_bisect.jsonl` as it starts and
ends, so after a hang that took the PC down, running it again records that run and
carries on:

```
python tools/hang_bisect.py --rounds 3
python tools/hang_bisect.py --only baseline --rounds 5
python tools/hang_bisect.py --summary
```

`tests/game/pacing_song.b3t` plays 20th Century Boy and measures the native
renderer's pacing over 20 s of it (`native_view stats` and `present_stats`). Its
header has the launch line; restore the window off-screen first (`window offscreen`),
since a minimized window doesn't paint.
