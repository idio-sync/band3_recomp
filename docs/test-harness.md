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
python tools/band3ctl.py window shot out.png  # what the window presents, overlays and all
```

`launch` starts the game minimized without taking focus (`--show` to watch it) and muted
(`--sound` to hear it), and
gives it its own saves in `out/test_user_data`, so tests never touch your profile. A
`.b3t` script is these commands one per line, with `#` comments; `run` stops at the
first one that fails, saves a screenshot of the moment and exits 1. Commands joined
with `;` share one connection, which `hold` needs: band3 lets go of everything held
when a client disconnects.

`capture` and `native_view` check the native view's rendering; see
[Render checks](native-renderer.md#render-checks).

`window` works on the game's window itself rather than through the harness, and never
activates it or puts it on a monitor. A window launched minimized never paints, so `window
offscreen` restores it to the right of every monitor, where it paints unseen; `window
shot <png>` then saves its client area as the window presents it, the SDK's overlays
included (`screenshot` is the game's picture alone); `window size <W>x<H>` sets
its client area in physical pixels, though Windows keeps it no bigger than the monitors
(`clamped` in the reply); `window minimize` minimizes it again, and moves where it
restores to back onto the primary monitor. `window status` prints where it is.

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms]` | press and let go (100 ms); join inputs with `+` |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `state` | the screen, song, venue, band, frame count, each player's instrument, the band's `score` (the song's scoreboard, 0 when a song starts) and, while `usb_mics` records, the `mics` slots: what records each, whether the game connected it and the bytes `fed` to it |
| `pad [player]` | the buttons, triggers and sticks the game reads from a player (1-4) |
| `wait <condition> [timeout=30s]`, `expect <condition> [timeout=5s]` | `screen=`, `screen~` (contains), `in_game`, `menus`, `song=<shortname>`, `frames=<n>`, `score>=<n>`, `mic=<slot>` (connected and fed audio since the wait began) |
| `sleep <n>s\|<n>ms` | waits that long, up to 600 s: `frames=` counts the main thread's frames, which stand still while the boot logos play on RB3's splash thread |
| `screenshot [emulated\|native] [name]` | the picture the window shows: the emulated GPU's at the game's size, or with `renderer` native the [native renderer](native-renderer.md)'s next frame at the size it draws at (the window's picture's, minimized too). `emulated` or `native` takes that one whatever the window shows; `native` while the native renderer is off draws the game's next frame once at 1280x720. The reply has `renderer` (`emulated` or `native`), `width` and `height` |
| `capture [name] [composed]` | a screenshot (`<name>.png`) and the native view's capture (`<name>.cap`) of the same full frame, and the native view's GPU backend drawing it (`<name>.gpu.png`), all under `screenshots/`. With `composed` it fails unless the capture is a post frame composed with the world frame before it (`proc_cmds` 2, `composed` true); its files are written either way. The reply's fields are under [Render checks](native-renderer.md#render-checks) |
| `native_view on [<width>x<height>] [nopost]\|off\|stats` | the native view live, as F9 draws it (same worker, same `native_view_backend`) but without its window, at 1280x720 unless given a size, and without RB3's post-processing with `nopost` (`post` in the reply), to see what it costs; `off` stops it and the capturing with it. While `renderer` is native the native renderer draws at the window's size, so `on` measures it at that size and `on` with a size is an error. Each reply has `stats`: the backend, frames `captured` and `rendered` since `on`, `skipped_busy` (captured while it was still drawing another), `worldless` (drawn without a world: with even/odd rendering, a frame that drew none and wasn't composed with the one before; 0 unless that's broken), each drawn frame's time in `ms` (`mean`, `p50`, `p95`, `max`; on the GPU the whole frame, uploads and reading back included) and the part after submitting in `wait_ms`, and the game's own `game_frames` and `game_fps` since the last `on` or `off`, and `rt_recording`: whether `native_view_record_targets` is `on`, and the texture passes the game drew in that time while capture was off, how many of them were recorded (those into textures that aren't drawn every frame or every world frame), their draws, and the game thread's `ms` recording them. `off`'s reply is the run it ends; `run` prints replies with `stats`. `tests/game/render_song_live.b3t` measures a song with it |
| `set <setting> <value>` | Band3 settings only |
| `bind <name>` | presses the key a key bind is set to, as if the window had focus: `bind instrument_lab` (F6), `bind settings` (F4), `bind native_view` (F9), `bind renderer` (F8); the `bind_` prefix is optional. With `window offscreen`, `window shot` then captures the overlay it opens |
| `quit` | |

Each player can have a virtual instrument: start a controller command with `p2`,
`p3` or `p4` for that player's (`p2 instrument drums`, `p2 hit red_pad`, `pad 2`);
without one it's player 1's. `state` lists every player's instrument.
`tests/game/multiplayer.b3t` plays a two-player song. Players 2-4 aren't signed in,
so they join as guests (`skip_profile_prompt`, on by default). With it off, RB3 asks
each to choose a profile when they join: No Profile plays as a guest.

`tests/game/liveless.b3t` takes two band3s on this PC online with Liveless, one
hosting and one joining it (its header has the two launches: each needs its own
`--port`, and the second its own `--user-data` and `liveless_port`).

`tests/game/usb_mic.b3t` sings a song's vocals through the USB mics' test tone (launch
with `-- --usb_mics=true --usb_mic_test_tone=220`) and waits for the score to go up.

Every instrument has `a b x y start back up down left right`; guitars add `green red
yellow blue orange strum_up strum_down solo`, drums `red_pad yellow_pad blue_pad
green_pad yellow_cym blue_cym green_cym kick kick2`, keys `overdrive`. Presses during
a screen transition are dropped, so wait for the screen you act on (and give a dialog
a moment to finish appearing). Submenus inside one screen (Play Now, Quickplay) don't
change the screen name. A song counts as `in_game` until you leave its results, and
`state` keeps the last song's details after that. Switching `instrument` reconnects
it, so press Start to join again, as a player would.
