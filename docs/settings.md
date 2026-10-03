# Settings, folders and songs

## The settings menu

Press **F4** in game to open the settings menu. band3's own options are under the
**Band3** categories (Game, Graphics, Integrations, Online, MIDI drums, Microphones, Debug), next
to the SDK's window, graphics, audio and input settings. Settings marked as needing a
restart take effect the next time the game starts; the others apply immediately, or from
the next time the game loads what they affect (for example, a forced venue applies from
the next venue load).

Without a keyboard, hold both stick clicks on a controller for a second to open the
settings menu, or both stick clicks and the left bumper for the
[Instrument Lab](instruments.md#instrument-lab). The same chord closes them.
`menu_shortcut` (Band3 → Game) turns this off. It reads controllers through SDL, so it
doesn't work with `input_backend = xinput`.

## Config files

**Save to config** writes the changed settings to `band3.toml` next to the executable.
Any setting can also be passed on the command line, e.g. `--forced_venue=arena_04`.

`band3_config.ini` is still read and documents every option. Where the same setting is
set in more than one place, the command line wins over `band3.toml`, which wins over
`band3_config.ini`.

Some options worth knowing about, by the names F4 and the command line use:

| Option | |
|---|---|
| `refresh_rate` | the rate the game runs at, e.g. 120 for a 120 Hz monitor (0 keeps the console's 60) |
| `background_fps` | the venue's frame rate: 0 keeps the venue's own (30 in most) at any `refresh_rate`, or set one, up to `refresh_rate`. Rates that divide `refresh_rate` (30 at 120, 180 or 240) draw evenly |
| `forced_venue` | a venue, a class of venues, or a comma-separated mix to pick from at random |
| `song_speed`, `track_speed` | play songs faster or slower, or scroll the highway faster |
| `controller_type`, `input_backend` | what gamepads play as, and SDL (the default) or XInput |
| `fast_start`, `disable_metamusic`, `lang`, `username` | skip the splash screens, silence the menu music, force a language, set the displayed name |
| `unlock_clothing`, `gold_on_all_difficulties` | RB3Enhanced's unlock options; see [Integrations](integrations.md#rb3enhanced-compatibility) |
| `autosave` | off keeps play out of your profile; it then only saves from the options menu |
| `skip_profile_prompt` | on (the default), players who join without a profile join as guests at once; off, the game asks each to choose a profile, as on a console |

## Folders

Files the game writes to its own folder (`game:\`) go to `game` in the user data root
(`Documents\band3\game` on Windows, or under `--user_data_root`), and the game reads them
back from there; `assets` is never written. Rock Band 3 Deluxe keeps its settings,
modifiers and playlists there (`dx_settings.dta`, `dx_playlist.dta` and so on), so
deleting that folder resets them. Test runs have their own user data, so they don't
change yours.

`band3_config.ini` can also move band3's other folders: `user_data_root` (saves, profiles
and the `game` folder; `Documents\band3` by default) and `cache_root` (the shader cache;
`cache` in the user data folder by default). A relative path there is relative to the
ini's folder, so `user_data_root = user_data` keeps everything band3 writes beside it, for
a portable install. band3 looks for the ini in its working directory, then beside the
executable.

## DLC and custom songs

On Windows for now, band3 reads DLC and custom songs (Xbox 360 `CON`, `LIVE` and `PIRS`
packages, Rock Band and Rock Band 2 DLC included, as RB3 reads them) straight from the
folders `content_folders` names (Band3 → Game, or `[game]` in `band3_config.ini`);
nothing is installed or unpacked, and band3 never writes there. Separate folders with `|`;
subfolders count, and a relative folder is relative to the ini's folder (`songs` beside it
by default). Changes apply at the next launch, except songs the web page's RhythmVerse tab
downloads or finds added ([RhythmVerse](integrations.md#rhythmverse)), which the game takes
in as it runs; a song taken out stays listed until the next launch.

A network folder works, but a local one is safer for audio: a song streams from its
package while it plays. A local folder is also faster to read: a large library on a slow
network folder can delay the song list on the first boot, and one that takes over a minute
isn't listed that session.

A custom song whose `song_id` is text instead of a number gets the number RB3Enhanced
gives it (the text's CRC-32 mod 9999999, plus 2130000000), so IDs agree with
RB3Enhanced's.

## Steam Deck

On a Steam Deck, band3 starts fullscreen and letterboxed (the game is 16:9, the screen
16:10), with vsync on and the FPS counter off, since Steam's performance overlay does
that job. These only fill in settings that `band3.toml` and the command line leave unset,
but they win over `band3_config.ini`, whose window settings are for a desktop. Turn
`steam_deck_defaults` off (Band3 → Game, then restart) to go back to the ini's.

band3 recognises a Deck from the `SteamDeck=1` Steam sets, or on Linux from the Deck's
hardware ids. Keep `resolution_scale` at 1: the Deck's screen can't show more than the
console's 720p, and scaling costs a lot of GPU time. The log warns when it's higher.

Instruments on a Deck:

- The Deck has one USB-C port, so more than one instrument or dongle needs a hub.
- For PlayStation and Wii dongles, install the
  [udev rules](instruments.md#playstation-and-wii-instruments-experimental) from Desktop
  Mode. SteamOS has no password for `sudo` until you set one with `passwd`.
- The log says what type each controller reports and what it plays as (for example
  "A controller reports type 1 (gamepad); playing as 7 (guitar)"). If an instrument
  shows up as a gamepad, turn off Steam Input for band3 (its controller settings in
  Steam) and try again, and include the log line if you report it.
