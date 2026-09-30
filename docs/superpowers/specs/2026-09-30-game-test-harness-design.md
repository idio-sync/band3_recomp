# Game test harness

A way for a developer, or Claude, to drive a running band3 and check a change in
game: press inputs, see what is on screen, read the game's state, and replay the
same steps as a regression run.

## Goals

- Check fixes live: launch the game, drive it a step at a time, look at
  screenshots and state.
- Repeatable regression runs: saved scripts that run unattended and pass or fail.
- Render checks: reach a screen or venue and capture the game's own frame.
- Instrument checks: drive guitar, drums, keys and pro guitar inputs.

Out of scope for v1: running RB3's own DTA script commands (phase 2; it plugs
into the same server as another command), an MCP wrapper, pixel comparison of
screenshots, more than one client at a time.

## Architecture

```
tools/band3ctl.py ──TCP 127.0.0.1:<test_port>──► band3.exe
  one command per call                           ├─ test server thread (src/Test/test_server.cpp)
  or `run script.b3t`                            ├─ TestPad   → VirtualInstrument
                                                 ├─ Snapshot  → presenter CaptureGuestOutput → PNG
                                                 └─ GameState ← rb3e_events hooks, frame counter
```

Inside band3, in `src/Test/`, compiled into every build and inert unless
`test_port` is set:

| Unit | Job |
|---|---|
| test_server | Listens on 127.0.0.1 only, one client at a time. A line of text in, a line of JSON out. Own thread; never calls guest code. |
| test_commands | Parses and runs commands against the interfaces below. Pure logic, unit tested without the game. |
| TestPad | Press, hold, release and axes on the existing virtual instrument (guitar, drums, keys, Mustang, Squier), put on player 1 for the session. A guitar navigates all of RB3's menus. |
| Snapshot | The game's frame from the presenter, written as a PNG to `screenshots/` next to the exe. |
| GameState | Locked record of screen name, in game or menus, song name/artist/shortname, venue, band info and a frame counter. Fed where `rb3e_events.cpp` already learns these, whether or not `events_enabled` is on. |

Outside band3:

- `tools/band3ctl.py`: standard-library Python. `launch` starts band3 with
  `--test_port` and waits for the server; single commands print the reply;
  `run <file.b3t>` replays a script and exits non-zero on failure.
- `tests/game/*.b3t`: saved regression scripts.

## Protocol

One command per line. Replies are one line of JSON: `{"ok":true,...}` or
`{"ok":false,"error":"...","state":{...}}`; a failure always carries the state.

Controller:

| Command | Effect |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | Which virtual instrument player 1 is (replugs it). |
| `press <inputs> [ms]` | Press and let go, default 100 ms. `+` joins inputs: `press green+red+strum_down`. |
| `hold <inputs>` / `release <inputs>\|all` | Hold until released. |
| `hit <target> [velocity] [fret]` | Drum pads and cymbals, keys 0-24, pro guitar strings. Velocity defaults to 100. |
| `axis whammy\|tilt <0..1>` | Guitar analog inputs. |

Input names: every instrument has `a b x y start back up down left right`;
guitar `green red yellow blue orange strum_up strum_down solo`; drums
`red_pad yellow_pad blue_pad green_pad yellow_cym blue_cym green_cym kick kick2`;
keys `overdrive`; pro guitar strings `low_e a d g b high_e` for `hit`. An input
the current instrument lacks is an error.

Observation:

| Command | Reply |
|---|---|
| `state` | `{screen, in_game, song:{name,artist,shortname}, venue, band, frame, instrument}` |
| `screenshot [name]` | `{path, width, height}`, written to `screenshots/<name or timestamp>.png` |
| `wait <condition> [timeout=30s]` | Returns once the condition holds, fails on timeout. |

Conditions: `screen=<exact>`, `screen~<substring>`, `in_game`, `menus`,
`song=<shortname>`, `frames=<n>` (n presented frames from now).

Control: `set <setting> <value>` for Band3/* settings only (the F4 menu's rules
on restarts apply); `quit` closes the game.

Scripts (`.b3t`) are the same commands, one per line, plus `#` comments and
`expect <condition>` (a `wait` with a 5 s default timeout). A failed `wait` or
`expect` stops the run and reports FAIL with the line number, the last state and
an automatic screenshot.

## Runtime

- Enabled by `--test_port=<port>` or `test_port` in band3.toml; 0 (default) is off.
- With the port set, band3 turns on `virtual_instrument` on player 1 for the
  session only, not saved; other controllers keep working around it.
- Input goes through VirtualInstrument's locked SetHeld/Pulse; state reads a
  locked GameState; `CaptureGuestOutput` may be called from any thread.
- `wait` polls GameState every ~16 ms. A disconnect releases every held input.
- Errors are replies, never crashes: unknown command, bad input name, input the
  instrument lacks, `set` outside Band3/*, screenshot before the first frame.
  A port already in use is logged and the game runs without the server. A second
  client is refused with an error line.
- Security: 127.0.0.1 only, off by default, no command reads or writes arbitrary
  paths; screenshot names are sanitised to a file name.

## Testing

- Unit tests (tests/, no SDK): command and input parsing, `+` combinations,
  per-instrument checks, `wait` condition matching against a fake GameState,
  the .b3t parser.
- The compile check covers the socket and presenter code.
- End to end: `tests/game/boot.b3t` launches, waits for the main menu, takes a
  screenshot and quits.
- README section next to the Instrument Lab's.
