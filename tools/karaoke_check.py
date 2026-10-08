#!/usr/bin/env python3
"""Checks band3's live pages against a game in a song with vocals: /live/events
and /lyrics?shortname=. Start band3 with the web server and get into a song
(tests/game/karaoke.b3t says how), then run this. It prints the line the page
shows at the end, to hold against a screenshot of the game.

Usage:
  python tools/karaoke_check.py [--port 21070] [--seconds 10] [--vocals lead]
Exits 1 at the first check that fails, saying which.
"""

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request


def read_events(url, seconds):
    """(arrival time, kind, data) for each event in `seconds` of the stream."""
    events = []
    with urllib.request.urlopen(url, timeout=15) as stream:
        end = time.monotonic() + seconds
        kind, data = None, []
        while time.monotonic() < end:
            line = stream.readline().decode('utf-8').rstrip('\r\n')
            if line.startswith('event: '):
                kind = line[len('event: '):]
            elif line.startswith('data: '):
                data.append(line[len('data: '):])
            elif line == '' and kind:
                events.append((time.monotonic(), kind, json.loads('\n'.join(data))))
                kind, data = None, []
    return events


def check(ok, what):
    if not ok:
        print(f'FAIL: {what}')
        sys.exit(1)
    print(f'ok: {what}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--port', type=int, default=21070)
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--vocals', default='lead', help='what state.vocals should say')
    args = parser.parse_args()
    server = f'http://127.0.0.1:{args.port}'

    events = read_events(f'{server}/live/events', args.seconds)
    states = [e for e in events if e[1] == 'state']
    clocks = [e for e in events if e[1] == 'clock']
    check(events and events[0][1] == 'state', 'the stream starts with the state')
    state = states[-1][2]
    check(state['in_game'] and state['song'], 'the game is in a song')
    check(not state['paused'], 'the song is playing')
    check(state['vocals'] == args.vocals, f"state.vocals is {args.vocals} ({state['vocals']})")
    check(len(clocks) >= args.seconds * 3, f'about 4 clock events a second ({len(clocks)})')
    ms = [c[2]['song_ms'] for c in clocks]
    check(all(b >= a for a, b in zip(ms, ms[1:])), 'the song clock never goes back')
    rate = (ms[-1] - ms[0]) / ((clocks[-1][0] - clocks[0][0]) * 1000)
    check(0.9 < rate < 1.1, f"the song clock keeps the song's time ({rate:.3f})")

    shortname = state['song']['shortname']
    with urllib.request.urlopen(f'{server}/lyrics?shortname={urllib.parse.quote(shortname)}',
                                timeout=15) as reply:
        lyrics = json.load(reply)
    check(lyrics['shortname'] == shortname and lyrics['parts'], f'{shortname} has lyrics')
    for part in lyrics['parts']:
        starts = [l['start_ms'] for l in part['lines']]
        check(starts == sorted(starts), f"{part['part']}'s lines are in order")
        check(all(l['start_ms'] <= s['start_ms'] and s['end_ms'] <= l['end_ms'] + 1000
                  for l in part['lines'] for s in l['syllables']),
              f"{part['part']}'s syllables sit in their lines")

    now = ms[-1]
    lead = lyrics['parts'][0]
    line = next((l for l in lead['lines'] if now < l['end_ms']), None)
    text = ''.join(s['text'] + ('' if s['join'] else ' ') for s in line['syllables']) if line else '-'
    print(f'at {now} ms the page shows ({lead["part"]}): {text.strip()}')


if __name__ == '__main__':
    main()
