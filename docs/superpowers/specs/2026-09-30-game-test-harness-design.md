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
| `hit <target> [velocity] [fret]` | Drum pads and cymbals, keys, pro guitar strings (fret 0-22). Velocity defaults to 100. |
| `axis whammy\|tilt <0..1>` | Guitar analog inputs. |

Input names: every instrument has `a b x y start back up down left right`;
guitar `green red yellow blue orange strum_up strum_down solo`; drums
`red_pad yellow_pad blue_pad green_pad yellow_cym blue_cym green_cym kick kick2`;
keys `overdrive`; pro guitar strings `low_e a_str d_str g_str b_str high_e` for `hit`, keys `key0`-`key24`. An input
the current instrument lacks is an error.

Observation:

| Command | Reply |
|---|---|
| `state` | `{screen, in_game, song:{name,artist,shortname}, venue, band, frame, instrument}` |
| `pad [player]` | `{connected, buttons, lt, rt, lx, ly, rx, ry, packet}` as the input system gives the game (added while testing: the only way to see what a hold really sends) |
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

## Multiplayer (added 2026-09-30)

Scripts drive up to four virtual instruments, one per player, to test local
multiplayer: joining several players, a song with several parts, the band state
per slot.

Instruments:

- `VirtualInstrument` becomes one per player (`VirtualInstrument::ForPlayer(n)`,
  n = 1-4), each with its kind, held inputs, pulses and whether it is plugged in.
- The settings (`virtual_instrument`, `_type`, `_player`) still describe one
  instrument, the Instrument Lab's; the harness sets `_player` to 1, so that one is
  the harness's player 1. Players 2-4 are plugged in by the harness only and never
  reach the settings. The Lab moving its instrument onto a player the harness uses
  takes that player over.
- One driver reports every plugged-in instrument as its own device (own id, guid
  tagged with the player). Each replugs on its own when its type changes.
- The player assignment pins each virtual instrument to its player; real pads fill
  the free slots in order; synthetic devices stay off player 1 while it has a
  virtual instrument. The assignment's slot logic moves into a pure function so it
  is unit tested.

Commands:

- `p1`-`p4` may start `instrument`, `press`, `hold`, `release`, `hit`, `axis` and
  the new `unplug`; no prefix means player 1. A prefix on `state`, `wait`,
  `expect`, `screenshot`, `set` or `quit` is an error. `pad` keeps its argument
  (`pad 2`) and also takes the prefix (`p2 pad`).
- `pN instrument <kind>` plugs player N in, or replugs it as that kind;
  `pN unplug` removes it. Driving an empty player is an error that says how to
  plug one in; input errors name the player.
- `state` reports `instruments`: four entries, the kind or null, replacing
  `instrument`.
- A client disconnecting releases held inputs on every player; instruments stay
  plugged in until unplugged or the session ends.

Testing: unit tests for the prefix, per-player targeting, empty players,
`instruments`, disconnect and the assignment; in game, three players (guitar,
drums, keys) joined from the menu, a player's type changed without affecting the
others, a three-part song on autoplay with `band` showing each member, and
`tests/game/multiplayer.b3t` passing from a fresh launch.

Out of scope: more than four players, players 2-4 in the Instrument Lab, online
play, saving the extra instruments.
