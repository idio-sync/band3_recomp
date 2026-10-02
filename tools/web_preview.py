#!/usr/bin/env python3
"""Serve band3's web page without the game, to look at it while changing it.

The page comes from src/Net/http_page.h on every load, so an edit shows on a
refresh, without a rebuild. The songs and their album art come from the game
data as the game finds them: a loose file in the game data root first, then
the title update's archive (patch_xbox.hdr), then the main one. Select can't
work without the game; it answers 409.

Usage:
  python tools/web_preview.py                 open http://127.0.0.1:21080/
  python tools/web_preview.py --port 8000 --address 0.0.0.0
  python tools/web_preview.py --game-data D:/rb3
"""

import argparse
import http.server
import os
import struct
import sys
import threading
import urllib.parse
import zlib

import read_game_config

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(REPO, 'src', 'Net', 'http_page.h')
GAME_DATA = os.path.join(REPO, 'assets')


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


def songs_from_dtb(tree, has_dir, has_file):
    """The songs.dtb songs the Music Library shows, with what /list_songs lists
    and their album art's file, if any. Left out, as the game leaves them out:
    songs whose folder isn't there (has_dir('songs/x/')), the trainers' lessons,
    which have no title, and test songs marked fake. The art is wherever its
    file is (has_file), whatever album_art says: the game shows Radar Love's
    though its entry says FALSE."""
    songs = []
    for entry in tree:
        if not isinstance(entry, list) or not entry or not isinstance(entry[0], str):
            continue
        fields = {f[0]: f[1:] for f in entry[1:] if isinstance(f, list) and f}
        song = {f[0]: f[1] for f in fields.get('song', []) if isinstance(f, list) and len(f) > 1}
        path = song.get('name')
        if (not isinstance(path, str) or 'name' not in fields or
                fields.get('fake', [None])[0] == 'TRUE' or
                not has_dir(path.rpartition('/')[0] + '/')):
            continue

        def text(key):
            value = fields.get(key, [''])[0]
            return to_utf8(value) if isinstance(value, str) else str(value)

        art = xbox_bitmap_path(path + '_keep.png')
        songs.append({'shortname': entry[0], 'title': text('name'), 'artist': text('artist'),
                      'album': text('album_name'), 'origin': text('game_origin'),
                      'art': art if has_file(art) else None})
    return songs


def format_songs(songs):
    """RB3E's /list_songs: a [shortname] section of key=value lines per song."""
    out = []
    for s in songs:
        lines = [f'[{s["shortname"]}]', f'shortname={s["shortname"]}']
        for key in ('title', 'artist', 'album', 'origin'):
            lines.append(f'{key}=' + s[key].replace('\r', ' ').replace('\n', ' '))
        out.append('\r\n'.join(lines) + '\r\n\r\n')
    return ''.join(out)


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


class Preview:
    def __init__(self, game):
        self.game = game
        self.songs = songs_from_dtb(
            read_game_config.parse_dtb(read_game_config.decrypt(game.read('songs/gen/songs.dtb'))),
            game.has_dir, game.has_file)
        self.by_shortname = {s['shortname']: s for s in self.songs}
        self.list_songs = format_songs(self.songs).encode()
        self.art = {}
        self.lock = threading.Lock()

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
            elif path.startswith('/album_art?shortname='):
                image = preview.album_art(path[len('/album_art?shortname='):])
                if image:
                    self.reply(200, 'image/png', image, max_age=3600)
                else:
                    self.reply(404, text, b'No album art for that shortname')
            elif path.startswith('/jump?shortname='):
                self.reply(409, text, b'This is web_preview.py: there is no game to select it in')
            else:
                self.reply(404, text, b'Not Found')

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
    args = ap.parse_args()

    preview = Preview(GameData(args.game_data))
    server = http.server.ThreadingHTTPServer((args.address, args.port), handler(preview))
    print(f'{len(preview.songs)} songs; open http://127.0.0.1:{args.port}/ (Ctrl+C stops it)',
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
