#!/usr/bin/env python3
"""Checks band3's live pages against a game in a song with vocals: /live/events
and /lyrics?shortname=. Start band3 with the web server and get into a song
(tests/game/karaoke.b3t says how), then run this. It prints the line the page
shows at the end, to hold against a screenshot of the game. It also checks
what the page's RB3 look needs: the clock's score, the state's band, each
line's overdrive flag, and every texture the page asks for (the header's normal
map is Rock Band 3 Deluxe's, so a 404 for it is reported, not failed).

Usage:
  python tools/karaoke_check.py [--port 21070] [--seconds 10] [--vocals lead]
Exits 1 at the first check that fails, saying which.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PAGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'src', 'Net', 'karaoke_page.h')


def fetch_status(url, attempts=6, opener=urllib.request.urlopen, sleep=time.sleep):
    """(status, content type) of a GET, asked again while the game is busy
    (503), as the page does: 1, 2, 4, 8, then 10 s apart."""
    for attempt in range(attempts):
        try:
            with opener(url, timeout=30) as reply:
                return reply.status, reply.headers.get('Content-Type', '')
        except urllib.error.HTTPError as e:
            if e.code != 503 or attempt == attempts - 1:
                return e.code, ''
        sleep(min(2 ** attempt, 10))


def page_textures():
    """(key, URL path) of each texture in the page's model (KaraokeModel's
    TEXTURES), read out of karaoke_page.h as the page has them."""
    source = open(PAGE, encoding='utf-8').read()
    names = dict(re.findall(r"var (k\w+) = '([^']+)';", source))
    out = []
    for key, body in re.findall(r"^\s+(\w+): \{ (milo: [^}]*|path: [^}]*) \}", source, re.M):
        milo = re.match(r"milo: (\w+|'[^']+'), name: '([^']+)'", body)
        if milo:
            where = names.get(milo.group(1), milo.group(1).strip("'"))
            out.append((key, '/game_asset/milo_texture?milo=' + urllib.parse.quote(where, safe='') +
                        '&name=' + urllib.parse.quote(milo.group(2), safe='')))
        else:
            path = re.match(r"path: '([^']+)'", body).group(1)
            out.append((key, '/game_asset/texture?path=' + urllib.parse.quote(path, safe='')))
    return out


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

    check(all(isinstance(l.get('overdrive'), bool) for p in lyrics['parts'] for l in p['lines']),
          'every line says whether it is an overdrive line')
    print(f"overdrive lines: {[(p['part'], sum(l['overdrive'] for l in p['lines'])) for p in lyrics['parts']]}")
    check(all(isinstance(c[2].get('score'), int) and c[2]['score'] >= 0 for c in clocks),
          f"every clock carries the band's score ({clocks[-1][2].get('score')})")
    band = state.get('band')
    check(isinstance(band, list) and len(band) == 4, 'the state has the band, four slots')
    check(any(m and m.get('part') == 'vocals' for m in band), f'the band has a singer ({band})')

    textures = page_textures()
    check(len(textures) >= 16, f'the page asks for {len(textures)} textures')
    for key, path in textures:
        status, kind = fetch_status(server + path)
        if key == 'header_normal' and status == 404:
            print(f'note: {key} is 404 (no Rock Band 3 Deluxe): the header is plain')
            continue
        check(status == 200 and kind == 'image/png', f'texture {key}: {status} {kind}')

    now = ms[-1]
    lead = lyrics['parts'][0]
    line = next((l for l in lead['lines'] if now < l['end_ms']), None)
    text = ''.join(s['text'] + ('' if s['join'] else ' ') for s in line['syllables']) if line else '-'
    print(f'at {now} ms the page shows ({lead["part"]}): {text.strip()}')


if __name__ == '__main__':
    main()
