#!/usr/bin/env python3
"""Read files out of RB3's archive (assets/gen/main_xbox.hdr and its .ark parts).

The archive is version 6: the header is encrypted with the DTB cipher (a
4-byte seed, then each byte XORed with the low byte of a Park-Miller style
generator), and lists every file's global offset across the parts. Config
.dtb files inside are encrypted the same way, each with its own seed, and
hold DataArrays in binary; --print turns one back into .dta-style text.

Usage:
  python tools/read_game_config.py --list [substring]
  python tools/read_game_config.py --print config/gen/sound.dtb
  python tools/read_game_config.py --extract config/gen/sound.dtb --out sound.dtb

Values worth knowing, for where band3 relies on them:
  config/gen/sound.dtb                     ratio_sliders: mic gain per mic type
  ../../system/run/config/gen/default.dtb  synth mic_types: XMic gain ranges
"""

import argparse
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(REPO, 'assets', 'gen')

# DTB node types
INT, FLOAT, VAR, SYMBOL, UNHANDLED, IFDEF, ELSE, ENDIF = 0, 1, 2, 5, 6, 7, 8, 9
ARRAY, COMMAND, STRING, PROPERTY = 0x10, 0x11, 0x12, 0x13
DIRECTIVES = {IFDEF: '#ifdef', 0x20: '#define', 0x21: '#include', 0x22: '#merge',
              0x23: '#ifndef', 0x24: '#autorun', 0x25: '#undef'}
BRACKETS = {ARRAY: '()', COMMAND: '{}', PROPERTY: '[]'}


def next_key(key):
    key = (key - (key // 0x1F31D) * 0x1F31D) * 0x41A7 - (key // 0x1F31D) * 0xB14
    return key + 0x7FFFFFFF if key <= 0 else key


def decrypt(data):
    key = struct.unpack_from('<i', data)[0]
    out = bytearray(len(data) - 4)
    for i, b in enumerate(data[4:]):
        key = next_key(key)
        out[i] = b ^ (key & 0xFF)
    return bytes(out)


class Ark:
    def __init__(self, gen=GEN):
        self.gen = gen
        with open(os.path.join(gen, 'main_xbox.hdr'), 'rb') as f:
            h = decrypt(f.read())
        self.pos = 0

        def i32():
            v = struct.unpack_from('<i', h, self.pos)[0]
            self.pos += 4
            return v

        version = i32()
        if version != 6:
            sys.exit(f'archive version {version}; only 6 (RB3) is known')
        i32()               # unknown, 1
        self.pos += 16      # hash
        i32()               # part count
        self.part_sizes = [struct.unpack_from('<I', h, self.pos + 4 * i)[0] for i in range(i32())]
        self.pos += 4 * len(self.part_sizes)
        self.part_names = []
        for _ in range(i32()):
            n = i32()
            self.part_names.append(h[self.pos:self.pos + n].decode())
            self.pos += n
        checksums = i32()
        self.pos += 4 * checksums
        blob_size = i32()
        blob = h[self.pos:self.pos + blob_size]
        self.pos += blob_size
        offsets = [i32() for _ in range(i32())]

        def string(i):
            start = offsets[i]
            return blob[start:blob.index(b'\0', start)].decode('latin1')

        # name -> (global offset, size)
        self.files = {}
        for _ in range(i32()):
            offset, name, directory, size, _inflated = struct.unpack_from('<qiiII', h, self.pos)
            self.pos += 24
            self.files[f'{string(directory)}/{string(name)}'] = (offset, size)

    def read(self, name):
        if name not in self.files:
            sys.exit(f'{name} is not in the archive (try --list)')
        offset, size = self.files[name]
        part = 0
        while offset >= self.part_sizes[part]:
            offset -= self.part_sizes[part]
            part += 1
        with open(os.path.join(self.gen, os.path.basename(self.part_names[part])), 'rb') as f:
            f.seek(offset)
            return f.read(size)


class DtbPrinter:
    def __init__(self, data):
        self.d = data
        self.pos = 1    # version byte

    def take(self, fmt):
        v = struct.unpack_from(fmt, self.d, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return v

    def text(self):
        n = self.take('<i')
        s = self.d[self.pos:self.pos + n].decode('latin1')
        self.pos += n
        return s

    def node(self, depth):
        kind = self.take('<i')
        if kind == INT:
            return str(self.take('<i'))
        if kind == FLOAT:
            return repr(round(self.take('<f'), 6))
        if kind in BRACKETS:
            open_, close = BRACKETS[kind]
            return open_ + self.array(depth + 1) + close
        if kind in (UNHANDLED, ELSE, ENDIF):
            self.take('<i')
            return {UNHANDLED: 'kDataUnhandled', ELSE: '#else', ENDIF: '#endif'}[kind]
        s = self.text()
        if kind == STRING:
            return '"' + s.replace('"', '\\"') + '"'
        if kind == VAR:
            return '$' + s
        if kind == SYMBOL:
            return s
        if kind in DIRECTIVES:
            return f'{DIRECTIVES[kind]} {s}'
        return f'<type 0x{kind:X}: {s}>'

    def array(self, depth):
        count = self.take('<h')
        self.take('<i')     # line number
        items = [self.node(depth) for _ in range(count)]
        flat = ' '.join(items)
        return flat if len(flat) <= 100 else ('\n' + '  ' * depth).join(items)

    def print(self):
        return self.array(0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument('--list', nargs='?', const='', metavar='SUBSTRING',
                       help='list archive files, optionally only names containing SUBSTRING')
    group.add_argument('--print', metavar='FILE', help='print a .dtb as text')
    group.add_argument('--extract', metavar='FILE', help='write a file out as stored (decrypting a .dtb)')
    ap.add_argument('--out', help='where --extract writes (default: the file name)')
    ap.add_argument('--gen', default=GEN, help='directory holding main_xbox.hdr (default: assets/gen)')
    args = ap.parse_args()

    ark = Ark(args.gen)
    if args.list is not None:
        for name, (_, size) in sorted(ark.files.items()):
            if args.list in name:
                print(f'{size:>10}  {name}')
    elif args.print:
        print(DtbPrinter(decrypt(ark.read(args.print))).print())
    else:
        data = ark.read(args.extract)
        if args.extract.endswith('.dtb'):
            data = decrypt(data)
        out = args.out or os.path.basename(args.extract)
        with open(out, 'wb') as f:
            f.write(data)
        print(f'wrote {len(data)} bytes to {out}')


if __name__ == '__main__':
    main()
