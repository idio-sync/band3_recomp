#!/usr/bin/env python3
"""Serve band3's web page without the game, to look at it while changing it.

The page comes from src/Net/http_page.h on every load, so an edit shows on a
refresh, without a rebuild. The songs and their album art come from the game
data as the game finds them: a loose file in the game data root first, then
the title update's archive (patch_xbox.hdr), then the main one. Select can't
work without the game; it answers 409. /status makes up what the game is doing
(--status), to work on the page's banner. The RhythmVerse tab searches
RhythmVerse itself, but its downloads are made up: nothing is saved. So are its
updates: two made-up songs, whatever Check for updates finds.

Usage:
  python tools/web_preview.py                 open http://127.0.0.1:21080/
  python tools/web_preview.py --status playing
  python tools/web_preview.py --port 8000 --address 0.0.0.0
  python tools/web_preview.py --game-data D:/rb3
"""

import argparse
import http.server
import json
import os
import struct
import sys
import threading
import time
import urllib.parse
import urllib.request
import zlib

import read_game_config

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(REPO, 'src', 'Net', 'http_page.h')
GAME_DATA = os.path.join(REPO, 'assets')

# Rock Band 3 Deluxe's changes to songs.dtb's entries (genres, years...), which
# the game takes over them; absent without Deluxe
SONG_UPDATES = 'dx/song_updates/gen/songs_updates.dtb'

# the parts /song_details rates, in its order
PARTS = ('band', 'guitar', 'bass', 'drum', 'vocals', 'keys', 'real_guitar', 'real_bass', 'real_keys')

# The game's difficulty tiers: SongMgr::RankTier gives the first of a part's
# thresholds its rank is at or under, 0 (Warmup) to 6 (Impossible), and 6 past
# the last. Read out of the running game (the SongMgr's list at +296), as no
# config file holds them.
THRESHOLDS = {
    'band': (162, 214, 242, 266, 291, 344, 500),
    'guitar': (138, 175, 220, 266, 332, 408, 474),
    'vocals': (131, 174, 217, 278, 352, 426, 500),
    'drum': (123, 150, 177, 241, 344, 447, 550),
    'bass': (134, 180, 227, 292, 363, 435, 500),
    'keys': (152, 210, 268, 326, 384, 442, 500),
    'real_guitar': (149, 204, 263, 322, 381, 441, 500),
    'real_bass': (149, 207, 266, 324, 383, 441, 500),
    'real_keys': (152, 210, 268, 326, 384, 442, 500),
}


def tier(part, rank):
    for i, threshold in enumerate(THRESHOLDS[part]):
        if rank <= threshold:
            return i
    return len(THRESHOLDS[part]) - 1


def index_page(header):
    """kIndexPage's raw string out of http_page.h's text."""
    return header.split('R"html(', 1)[1].split(')html"', 1)[0]


def xbox_bitmap_path(path):
    """The file the game builds a texture path into: songs/x/x_keep.png is
    songs/x/gen/x_keep.png_xbox."""
    directory, _, name = path.rpartition('/')
    return f'{directory}/gen/{name}_xbox' if directory else f'gen/{name}_xbox'


def to_utf8(text):
    """The game's strings are UTF-8 for songs that say so and Latin-1 for the
    rest; parse_dtb reads them all as Latin-1."""
    raw = text.encode('latin1')
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        return text


def songs_from_dtb(tree, has_dir, has_file, genres=None, updates=None):
    """The songs.dtb songs the Music Library shows, with what /list_songs lists,
    what /song_details does (genre names from `genres`) and their album art's
    file, if any. `updates` (entries like songs.dtb's) replace the fields they
    name, as Rock Band 3 Deluxe's song updates do in the game. Left out, as the
    game leaves them out: songs whose folder isn't there (has_dir('songs/x/')),
    the trainers' lessons, which have no title, and test songs marked fake. The
    art is wherever its file is (has_file), whatever album_art says: the game
    shows Radar Love's though its entry says FALSE."""
    genres = genres or {}
    changes = {}
    for entry in updates or []:
        if isinstance(entry, list) and entry and isinstance(entry[0], str):
            changes.setdefault(entry[0], {}).update(
                {f[0]: f[1:] for f in entry[1:] if isinstance(f, list) and f})
    songs = []
    for entry in tree:
        if not isinstance(entry, list) or not entry or not isinstance(entry[0], str):
            continue
        fields = {f[0]: f[1:] for f in entry[1:] if isinstance(f, list) and f}
        fields.update(changes.get(entry[0], {}))
        song = {f[0]: f[1] for f in fields.get('song', []) if isinstance(f, list) and len(f) > 1}
        path = song.get('name')
        if (not isinstance(path, str) or 'name' not in fields or
                fields.get('fake', [None])[0] == 'TRUE' or
                not has_dir(path.rpartition('/')[0] + '/')):
            continue

        def text(key):
            value = fields.get(key, [''])[0]
            return to_utf8(value) if isinstance(value, str) else str(value)

        def number(value):
            return value if isinstance(value, int) else 0

        ranks = {r[0]: r[1] for r in fields.get('rank', []) if isinstance(r, list) and len(r) > 1}
        genre = text('genre')
        details = {
            'genre': genres.get(genre, genre),
            'year': number(fields.get('year_released', [0])[0]),
            'length_ms': number(fields.get('song_length', [0])[0]),
            'vocal_parts': number(song.get('vocal_parts', 0)),
            # a rank of 0 is a part the song doesn't have
            'tiers': {p: tier(p, ranks[p]) for p in PARTS if number(ranks.get(p, 0)) > 0},
        }
        art = xbox_bitmap_path(path + '_keep.png')
        songs.append({'shortname': entry[0], 'title': text('name'), 'artist': text('artist'),
                      'album': text('album_name'), 'origin': text('game_origin'),
                      'art': art if has_file(art) else None, 'details': details})
    return songs


def format_details(songs):
    """/song_details: each song's details, by shortname."""
    return json.dumps({s['shortname']: s['details'] for s in songs}, separators=(',', ':'))


def demo_status(kind, songs, seconds):
    """/status as band3 would answer it: 'menu', 'library' (the Music Library
    open), or 'playing' the first song, round and round, `seconds` in."""
    if kind == 'playing' and songs:
        song = songs[0]
        length = song['details']['length_ms'] or 180000
        position = int(seconds * 1000) % length
        return {'screen': 'game_screen', 'in_library': False,
                'playing': {'shortname': song['shortname'], 'title': song['title'],
                            'artist': song['artist'], 'score': position * 2,
                            'position_ms': position, 'length_ms': length}}
    if kind == 'library':
        return {'screen': 'song_select_screen', 'in_library': True, 'playing': None}
    return {'screen': 'main_hub_screen', 'in_library': False, 'playing': None}


def format_songs(songs):
    """RB3E's /list_songs: a [shortname] section of key=value lines per song."""
    out = []
    for s in songs:
        lines = [f'[{s["shortname"]}]', f'shortname={s["shortname"]}']
        for key in ('title', 'artist', 'album', 'origin'):
            lines.append(f'{key}=' + s[key].replace('\r', ' ').replace('\n', ' '))
        out.append('\r\n'.join(lines) + '\r\n\r\n')
    return ''.join(out)


RHYTHMVERSE = 'https://rhythmverse.co'
RV_PAGE_SIZE = 25
# RhythmVerse's difficulty fields by the game's part names; its 1-7 are the
# game's tiers 0-6, and 0 or -1 a part the song doesn't have
RV_PARTS = (('diff_band', 'band'), ('diff_guitar', 'guitar'), ('diff_bass', 'bass'),
            ('diff_drums', 'drum'), ('diff_vocals', 'vocals'), ('diff_keys', 'keys'),
            ('diff_proguitar', 'real_guitar'), ('diff_probass', 'real_bass'),
            ('diff_prokeys', 'real_keys'))


def _rv_number(value):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return 0


def rv_song(entry):
    """One song of RhythmVerse's search reply as band3's /rv/search gives it
    (src/Net/rhythmverse.cpp), or None without a usable file ID."""
    data = entry.get('data') or {}
    upload = entry.get('file') or {}
    file_id = str(upload.get('file_id') or '')
    if not file_id or len(file_id) > 64 or not all(c.isascii() and (c.isalnum() or c == '.')
                                                    for c in file_id):
        return None

    def field(own, song):
        value = upload.get(own)
        return data.get(song) if value is None or str(value) == '' else value

    def absolute(url):
        return RHYTHMVERSE + url if url.startswith('/') else url

    tiers = {}
    for key, part in RV_PARTS:
        value = _rv_number(field(key, key))
        if value >= 1:
            tiers[part] = min(value - 1, 6)
    url = upload.get('external_url') or upload.get('download_url') or ''
    host = urllib.parse.urlsplit(url).netloc
    hosted = bool(url) and host in ('', 'rhythmverse.co')
    download = hosted and not _rv_number(upload.get('zippata'))
    art = field('album_art', 'album_art') or ''
    return {
        'file_id': file_id,
        'title': str(field('file_title', 'title') or ''),
        'artist': str(field('file_artist', 'artist') or ''),
        'album': str(field('file_album', 'album') or ''),
        'genre': str(field('file_genre', 'genre') or ''),
        'author': str((upload.get('author') or {}).get('name') or upload.get('user') or ''),
        'year': _rv_number(field('file_year', 'year')),
        'length_ms': _rv_number(field('song_length', 'song_length')) * 1000,
        'vocal_parts': _rv_number(field('vocal_parts_authored', 'vocal_parts')),
        'size': _rv_number(upload.get('size')),
        # text song_ids, which band3 numbers as RB3E does, are left out here
        'song_id': _rv_number(upload.get('custom_id')),
        'downloads': _rv_number(upload.get('downloads')),
        'tiers': tiers,
        'art': absolute(art) if art else '',
        'page': absolute(upload.get('file_url') or ''),
        'host': '' if download else ('rhythmverse.co' if hosted else host),
        'download': download,
    }


RV_DOWNLOADABLE_PAGE_SIZE = 100
# the page's sorts, as RhythmVerse's field and order
RV_SORTS = {'newest': ('release_date', 'DESC'), 'updated': ('update_date', 'DESC'),
            'downloads': ('downloads', 'DESC'), 'title': ('title', 'ASC'),
            'artist': ('artist', 'ASC'), 'length': ('length', 'ASC')}
# RhythmVerse's names for the game's parts
RV_INSTRUMENTS = {'guitar': 'guitar', 'bass': 'bass', 'drum': 'drums', 'vocals': 'vocals',
                  'keys': 'keys', 'real_guitar': 'proguitar', 'real_bass': 'probass',
                  'real_keys': 'prokeys'}


def rv_search_form(query):
    """What band3 posts to RhythmVerse for /rv/search's query (a dict of
    strings, as src/Net/rhythmverse.cpp's ParseSearchOptions reads it):
    (url, form as a list of pairs, page size)."""
    text = query.get('text', '')
    try:
        page = max(int(query.get('page', '1')), 1)
    except ValueError:
        page = 1
    downloadable = query.get('downloadable') == '1'
    size = RV_DOWNLOADABLE_PAGE_SIZE if downloadable else RV_PAGE_SIZE
    form = [('records', size), ('page', page), ('data_type', 'full')]
    if text:
        url = RHYTHMVERSE + '/api/rb3xbox/songfiles/search/live'
        form.append(('text', text))
    else:
        url = RHYTHMVERSE + '/api/rb3xbox/songfiles/list'
    sort = query.get('sort', '') if query.get('sort') in RV_SORTS else ('' if text else 'newest')
    if sort:
        form += [('sort[0][sort_by]', RV_SORTS[sort][0]), ('sort[0][sort_order]', RV_SORTS[sort][1])]
    split = lambda key: [v for v in query.get(key, '').split(',') if v]
    form += [('instrument[]', RV_INSTRUMENTS[p]) for p in split('has') if p in RV_INSTRUMENTS]
    if query.get('harmonies') == '1':
        form += [('vocal_parts[]', '2'), ('vocal_parts[]', '3')]
    form += [('genre[]', g) for g in split('genre')
             if len(g) <= 32 and all(c == '_' or 'a' <= c <= 'z' for c in g)]
    form += [('decade[]', d) for d in split('decade')
             if d.isascii() and d.isdigit() and 1900 <= int(d) <= 2100 and int(d) % 10 == 0]
    part, _, tier = query.get('cap', '').partition(':')
    if part in RV_INSTRUMENTS and tier.isdigit() and int(tier) <= 6:
        form.append(('tierinstrument[]', RV_INSTRUMENTS[part]))
        form += [('tier[]', str(t + 1)) for t in range(int(tier) + 1)]
    return url, form, size


def rv_search_result(reply, downloaded):
    """band3's /rv/search out of RhythmVerse's reply, or None if it isn't one."""
    if not isinstance(reply, dict) or reply.get('status') != 'success':
        return None
    data = reply.get('data') or {}
    # a search that found nothing says "songs": false
    if not isinstance(data.get('songs'), list) and data.get('songs') is not False:
        return None
    songs = [s for s in map(rv_song, data['songs'] or []) if s]
    for s in songs:
        s['downloaded'] = s['file_id'] in downloaded
        # there's no game to ask, so the page goes by artist and title; and no
        # downloads to have updates of
        s['in_library'] = None
        s['update'] = ''
    return {'total': _rv_number((data.get('records') or {}).get('total_filtered')),
            'page': max(_rv_number((data.get('pagination') or {}).get('page')), 1),
            'page_size': RV_PAGE_SIZE, 'songs': songs}


class FakeDownloads:
    """band3's downloads, made up: each takes a few seconds, is in the game as long
    again after, and nothing is saved."""

    SECONDS = 4

    def __init__(self):
        self.lock = threading.Lock()
        self.known = {}
        self.started = {}  # file_id -> (song, monotonic start)

    def remember(self, songs):
        with self.lock:
            self.known.update({s['file_id']: s for s in songs})

    def queue(self, file_id, update=False):
        """(status, text) as band3's POST /rv/download answers."""
        with self.lock:
            song = self.known.get(file_id)
            if not song:
                return 404, 'No search has found that song; search for it again'
            if not song['download']:
                return 409, "RhythmVerse doesn't host this one: download it from its page"
            if update and song.get('update') != 'available':
                return 409, 'RhythmVerse has nothing newer of it'
            if file_id in self.started:
                return 200, 'Updating' if update else 'Downloading'
            self.started[file_id] = (song, time.monotonic(), update)
            return 200, 'Updating' if update else 'Downloading'

    def downloaded(self):
        now = time.monotonic()
        with self.lock:
            return {i for i, (_, t, _) in self.started.items() if now - t >= self.SECONDS}

    def report(self):
        now = time.monotonic()
        out = []
        with self.lock:
            for file_id, (song, start, update) in self.started.items():
                total = song['size'] or 50 * 1048576
                part = min((now - start) / self.SECONDS, 1)
                # the game takes it in as long again after
                out.append({'file_id': file_id, 'title': song['title'], 'artist': song['artist'],
                            'state': 'done' if part >= 1 else 'downloading',
                            'song_id': song['song_id'], 'update': update,
                            'received': int(total * part), 'total': total, 'error': '',
                            'in_library': now - start >= 2 * self.SECONDS})
        return {'folder': 'songs\\rhythmverse (web_preview: nothing is saved)', 'downloads': out}


def fake_updates():
    """Made-up songs band3 downloaded that RhythmVerse has newer versions of,
    as /rv/updates gives them."""
    song = {'album': '', 'genre': 'Rock', 'author': 'web_preview', 'year': 2001,
            'length_ms': 200000, 'vocal_parts': 1, 'size': 30 * 1048576, 'downloads': 0,
            'tiers': {'band': 3, 'guitar': 4, 'bass': 2, 'drum': 3, 'vocals': 1},
            'art': '', 'page': RHYTHMVERSE + '/', 'host': '', 'download': True,
            'downloaded': True, 'song_id': 0, 'in_library': None, 'update': 'available'}
    return [dict(song, file_id='preview.1', title='A Made-Up Song', artist='The Previews'),
            dict(song, file_id='preview.2', title='Another Version', artist='Web Preview')]


class FakeUpdates:
    """band3's update check, made up: a check takes a few seconds and finds the
    same updates, which are downloaded once asked for (FakeDownloads)."""

    SECONDS = 3

    def __init__(self, downloads):
        self.downloads = downloads
        self.lock = threading.Lock()
        self.songs = fake_updates()
        downloads.remember(self.songs)
        self.checked = time.time() - 3 * 3600
        self.started = None  # when the check asked for began, monotonic

    def check(self):
        with self.lock:
            if self.started is None:
                self.started = time.monotonic()

    def report(self):
        with self.lock:
            checking = self.started is not None and time.monotonic() - self.started < self.SECONDS
            if self.started is not None and not checking:
                self.started = None
                self.checked = time.time()
        done = self.downloads.downloaded()
        updates = [dict(s, update='pending' if s['file_id'] in done else 'available')
                   for s in self.songs]
        return {'checking': checking, 'checked': int(self.checked), 'error': '',
                'downloads': 12, 'updates': updates}


def _from565(c):
    return [(c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31, 255]


def _mix(a, b, wa, wb):
    return [(a[i] * wa + b[i] * wb) // (wa + wb) for i in range(3)] + [255]


def decode_xbox_bitmap(data):
    """RB3's Xbox 360 bitmap (RndBitmap), as band3's src/Net/album_art.cpp
    decodes it: a 32-byte header, then DXT1 (8) or DXT5 (24) blocks with each
    16-bit word byte-swapped. (width, height, RGBA bytes), or None."""
    if len(data) < 32:
        return None
    encoding = struct.unpack_from('<I', data, 2)[0]
    width, height = struct.unpack_from('<HH', data, 7)
    if encoding not in (8, 24) or not width or not height or width % 4 or height % 4:
        return None
    block_size = 8 if encoding == 8 else 16
    if len(data) < 32 + block_size * (width // 4) * (height // 4):
        return None

    rgba = bytearray(width * height * 4)
    at = 32
    for by in range(height // 4):
        for bx in range(width // 4):
            block = bytearray(data[at:at + block_size])
            block[0::2], block[1::2] = block[1::2], block[0::2]
            at += block_size
            color = block if encoding == 8 else block[8:]
            c0, c1, indices = struct.unpack_from('<HHI', color)
            a, b = _from565(c0), _from565(c1)
            if c0 > c1 or encoding == 24:
                palette = [a, b, _mix(a, b, 2, 1), _mix(a, b, 1, 2)]
            else:
                palette = [a, b, _mix(a, b, 1, 1), [0, 0, 0, 0]]
            pixels = [list(palette[(indices >> (2 * i)) & 3]) for i in range(16)]
            if encoding == 24:
                a0, a1 = block[0], block[1]
                if a0 > a1:
                    alphas = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    alphas = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                bits = int.from_bytes(block[2:8], 'little')
                for i in range(16):
                    pixels[i][3] = alphas[(bits >> (3 * i)) & 7]
            for i, p in enumerate(pixels):
                out = ((by * 4 + i // 4) * width + bx * 4 + i % 4) * 4
                rgba[out:out + 4] = bytes(p)
    return width, height, bytes(rgba)


def png(width, height, rgba):
    """An RGBA PNG, with only the standard library."""
    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body +
                struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF))

    stride = width * 4
    rows = b''.join(b'\0' + rgba[y * stride:(y + 1) * stride] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


class GameData:
    """Files as the game finds them: loose in the game data root, then in the
    title update's archive, then the main one."""

    def __init__(self, root):
        self.root = root
        gen = os.path.join(root, 'gen')
        self.arks = []
        for header in ('patch_xbox.hdr', 'main_xbox.hdr'):
            if os.path.isfile(os.path.join(gen, header)):
                self.arks.append(read_game_config.Ark(gen, header))
        if not self.arks:
            sys.exit(f'no main_xbox.hdr in {gen}; pass --game-data')
        self.dirs = {name.rpartition('/')[0] + '/' for ark in self.arks for name in ark.files}

    def read(self, path):
        loose = os.path.join(self.root, *path.split('/'))
        if os.path.isfile(loose):
            with open(loose, 'rb') as f:
                return f.read()
        for ark in self.arks:
            if path in ark.files:
                return ark.read(path)
        return None

    def has_dir(self, directory):
        return directory in self.dirs or os.path.isdir(os.path.join(self.root, *directory.split('/')))

    def has_file(self, path):
        return (any(path in ark.files for ark in self.arks) or
                os.path.isfile(os.path.join(self.root, *path.split('/'))))


def genre_names(game):
    """The game's English names for things, as Locale::Localize looks them up:
    its locale file, then the title update's and Rock Band 3 Deluxe's additions."""
    names = {}
    for path in ('ui/locale/eng/gen/locale_keep.dtb', 'ui/locale/eng/gen/locale_updates_keep.dtb',
                 'dx/locale/gen/dx_locale_updates.dtb'):
        data = game.read(path)
        if not data:
            continue
        for entry in read_game_config.parse_dtb(read_game_config.decrypt(data)):
            if isinstance(entry, list) and len(entry) > 1 and isinstance(entry[1], str):
                names[entry[0]] = to_utf8(entry[1])
    return names


class Preview:
    def __init__(self, game, status='library'):
        self.game = game
        self.status = status
        self.started = time.monotonic()
        def dtb(path):
            data = game.read(path)
            return read_game_config.parse_dtb(read_game_config.decrypt(data)) if data else []

        self.songs = songs_from_dtb(dtb('songs/gen/songs.dtb'), game.has_dir, game.has_file,
                                    genre_names(game), dtb(SONG_UPDATES))
        self.by_shortname = {s['shortname']: s for s in self.songs}
        self.list_songs = format_songs(self.songs).encode()
        self.details = format_details(self.songs).encode()
        self.art = {}
        self.lock = threading.Lock()
        self.downloads = FakeDownloads()
        self.updates = FakeUpdates(self.downloads)

    def rv_search(self, query):
        """band3's /rv/search, asked of RhythmVerse: (status, body)."""
        url, form, size = rv_search_form(query)
        request = urllib.request.Request(
            url, data=urllib.parse.urlencode(form).encode(),
            headers={'User-Agent': 'band3 web_preview', 'Accept': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=30) as reply:
                result = rv_search_result(json.load(reply), self.downloads.downloaded())
        except (OSError, ValueError) as e:
            return 502, f"Couldn't reach RhythmVerse: {e}"
        if result is None:
            return 502, "RhythmVerse's reply wasn't one band3 can read"
        result['page_size'] = size
        if query.get('downloadable') == '1':
            result['songs'] = [s for s in result['songs'] if s['download']]
        self.downloads.remember(result['songs'])
        return 200, json.dumps(result)

    def album_art(self, shortname):
        song = self.by_shortname.get(shortname)
        if not song or not song['art']:
            return None
        with self.lock:
            if shortname not in self.art:
                data = self.game.read(song['art'])
                image = decode_xbox_bitmap(data) if data else None
                self.art[shortname] = png(*image) if image else None
            return self.art[shortname]


def handler(preview):
    class Handler(http.server.BaseHTTPRequestHandler):
        # the page shows album art only when the server says it's band3
        server_version = 'band3'
        sys_version = ''

        def reply(self, status, content_type, body, max_age=0):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', f'max-age={max_age}' if max_age else 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urllib.parse.unquote(self.path)
            text = 'text/plain; charset=utf-8'
            if path == '/':
                with open(PAGE, encoding='utf-8') as f:
                    self.reply(200, 'text/html; charset=utf-8', index_page(f.read()).encode())
            elif path == '/list_songs':
                self.reply(200, text, preview.list_songs)
            elif path == '/song_details':
                self.reply(200, 'application/json', preview.details)
            elif path == '/status':
                status = demo_status(preview.status, preview.songs,
                                     time.monotonic() - preview.started)
                self.reply(200, 'application/json', json.dumps(status).encode())
            elif path.startswith('/album_art?shortname='):
                image = preview.album_art(path[len('/album_art?shortname='):])
                if image:
                    self.reply(200, 'image/png', image, max_age=3600)
                else:
                    self.reply(404, text, b'No album art for that shortname')
            elif path.startswith('/jump?shortname='):
                self.reply(409, text, b'This is web_preview.py: there is no game to select it in')
            elif urllib.parse.urlsplit(self.path).path == '/rv/search':
                query = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(self.path).query))
                status, body = preview.rv_search(query)
                self.reply(status, 'application/json' if status == 200 else text, body.encode())
            elif path == '/rv/downloads':
                self.reply(200, 'application/json', json.dumps(preview.downloads.report()).encode())
            elif path == '/rv/updates':
                self.reply(200, 'application/json', json.dumps(preview.updates.report()).encode())
            else:
                self.reply(404, text, b'Not Found')

        def do_POST(self):
            text = 'text/plain; charset=utf-8'
            if self.path not in ('/rv/download', '/rv/check'):
                self.reply(405, text, b'Only GET is supported')
                return
            if not (self.headers.get('Content-Type') or '').startswith('application/json'):
                self.reply(415, text, b'Send the request as JSON')
                return
            length = int(self.headers.get('Content-Length') or 0)
            try:
                request = json.loads(self.rfile.read(length))
                file_id = str(request.get('file_id', ''))
                update = request.get('update') is True
            except (ValueError, AttributeError):
                file_id, update = '', False
            if self.path == '/rv/check':
                preview.updates.check()
                self.reply(200, text, b'Checking')
                return
            status, body = preview.downloads.queue(file_id, update)
            self.reply(status, text, body.encode())

        def log_message(self, *args):
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=21080)
    ap.add_argument('--address', default='127.0.0.1',
                    help='0.0.0.0 to reach it from other devices (default: this PC only)')
    ap.add_argument('--game-data', default=GAME_DATA,
                    help='the game data root, holding gen/main_xbox.hdr (default: assets)')
    ap.add_argument('--status', choices=('menu', 'library', 'playing'), default='library',
                    help="what /status says the game is doing (default: the Music Library's open)")
    args = ap.parse_args()

    preview = Preview(GameData(args.game_data), args.status)
    server = http.server.ThreadingHTTPServer((args.address, args.port), handler(preview))
    print(f'{len(preview.songs)} songs; open http://127.0.0.1:{args.port}/ (Ctrl+C stops it)',
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
