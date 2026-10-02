"""Tests for web_preview.py's song list and album art: python tools/test_web_preview.py"""

import struct
import unittest
import zlib

import web_preview


def bitmap(encoding, bpp, width, height, blocks):
    """A .png_xbox: the 32-byte header, then the blocks with each 16-bit word
    byte-swapped, as the 360 stores them."""
    header = bytearray(32)
    header[0], header[1], header[2] = 1, bpp, encoding
    struct.pack_into('<HHH', header, 7, width, height, width * bpp // 8)
    swapped = bytearray(blocks)
    swapped[0::2], swapped[1::2] = blocks[1::2], blocks[0::2]
    return bytes(header + swapped)


def dxt1_block(c0, c1, indices):
    return struct.pack('<HHI', c0, c1, indices)


def pixel(image, x, y):
    width, _, rgba = image
    i = (y * width + x) * 4
    return tuple(rgba[i:i + 4])


RED, BLUE = 0xF800, 0x001F


class AlbumArtTest(unittest.TestCase):
    def test_dxt1_decodes_its_four_colors(self):
        image = web_preview.decode_xbox_bitmap(bitmap(8, 4, 4, 4, dxt1_block(RED, BLUE, 0xE4)))
        self.assertEqual(image[:2], (4, 4))
        self.assertEqual(pixel(image, 0, 0), (255, 0, 0, 255))
        self.assertEqual(pixel(image, 1, 0), (0, 0, 255, 255))
        self.assertEqual(pixel(image, 2, 0), (170, 0, 85, 255))
        self.assertEqual(pixel(image, 3, 0), (85, 0, 170, 255))

    def test_dxt1_with_the_first_color_lower_has_transparent_black(self):
        image = web_preview.decode_xbox_bitmap(bitmap(8, 4, 4, 4, dxt1_block(BLUE, RED, 0xE4)))
        self.assertEqual(pixel(image, 2, 0), (127, 0, 127, 255))
        self.assertEqual(pixel(image, 3, 0), (0, 0, 0, 0))

    def test_dxt5_takes_alpha_from_its_alpha_block(self):
        alpha = bytes([255, 0]) + (0b010_001_000).to_bytes(6, 'little')
        image = web_preview.decode_xbox_bitmap(
            bitmap(24, 8, 4, 4, alpha + dxt1_block(BLUE, RED, 0xC0)))
        self.assertEqual(pixel(image, 0, 0), (0, 0, 255, 255))
        self.assertEqual(pixel(image, 1, 0), (0, 0, 255, 0))
        self.assertEqual(pixel(image, 2, 0), (0, 0, 255, 218))
        self.assertEqual(pixel(image, 3, 0), (170, 0, 85, 255))

    def test_blocks_go_left_to_right_then_down(self):
        blocks = b''.join(dxt1_block(c, 0, 0) for c in (RED, BLUE, 0x07E0, 0xFFFF))
        image = web_preview.decode_xbox_bitmap(bitmap(8, 4, 8, 8, blocks))
        self.assertEqual(pixel(image, 7, 3), (0, 0, 255, 255))
        self.assertEqual(pixel(image, 0, 7), (0, 255, 0, 255))

    def test_bitmaps_it_cant_decode_give_none(self):
        self.assertIsNone(web_preview.decode_xbox_bitmap(bitmap(8, 4, 8, 8, dxt1_block(RED, 0, 0))))
        self.assertIsNone(web_preview.decode_xbox_bitmap(bitmap(3, 32, 4, 4, bytes(64))))
        self.assertIsNone(web_preview.decode_xbox_bitmap(bitmap(8, 4, 6, 4, bytes(16))))
        self.assertIsNone(web_preview.decode_xbox_bitmap(b''))

    def test_the_games_path_becomes_the_file_it_builds(self):
        self.assertEqual(web_preview.xbox_bitmap_path('songs/rehab/rehab_keep.png'),
                         'songs/rehab/gen/rehab_keep.png_xbox')

    def test_png_holds_the_pixels(self):
        rgba = bytes(range(2 * 3 * 4))
        png = web_preview.png(2, 3, rgba)
        self.assertTrue(png.startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertEqual(struct.unpack('>II', png[16:24]), (2, 3))
        idat = png.index(b'IDAT')
        size = struct.unpack('>I', png[idat - 4:idat])[0]
        rows = zlib.decompress(png[idat + 4:idat + 4 + size])
        # each row: filter byte 0, then its pixels
        self.assertEqual(rows, b''.join(b'\0' + rgba[y * 8:(y + 1) * 8] for y in range(3)))


SONGS = [
    '#ifndef', ['rehab',
                ['name', 'Rehab'], ['artist', 'Amy Winehouse'], ['album_name', 'Back to Black'],
                ['song', ['name', 'songs/rehab/rehab']], ['game_origin', 'rb3']],
    ['noart', ['name', 'No Art'], ['artist', 'Band'], ['song', ['name', 'songs/noart/noart']]],
    # no files
    ['_budget_test', ['name', 'Test'], ['song', ['name', 'songs/_budget_test/_budget_test']]],
    # a trainer lesson: no title
    ['note_basics_1', ['song_id', 1201], ['song', ['name', 'songs/note_basics_1/note_basics_1']]],
    ['_invalid_version_test', ['name', 'Invalid'], ['fake', 'TRUE'],
     ['song', ['name', 'songs/_invalid_version_test/_invalid_version_test']]],
    ['mot', ['name', 'Mot\xc3\xb6rhead'], ['artist', 'Mot\xf6rhead'],
     ['song', ['name', 'songs/mot/mot']]],
]


class SongsTest(unittest.TestCase):
    def songs(self):
        return web_preview.songs_from_dtb(
            SONGS, lambda d: d != 'songs/_budget_test/',
            lambda f: f == 'songs/rehab/gen/rehab_keep.png_xbox')

    def test_lists_the_songs_the_music_library_shows(self):
        self.assertEqual([s['shortname'] for s in self.songs()], ['rehab', 'noart', 'mot'])

    def test_takes_the_fields_rb3e_lists(self):
        rehab = self.songs()[0]
        self.assertEqual((rehab['title'], rehab['artist'], rehab['album'], rehab['origin']),
                         ('Rehab', 'Amy Winehouse', 'Back to Black', 'rb3'))

    def test_album_art_where_its_file_is(self):
        songs = self.songs()
        self.assertEqual(songs[0]['art'], 'songs/rehab/gen/rehab_keep.png_xbox')
        self.assertIsNone(songs[1]['art'])

    def test_utf8_stays_and_latin1_becomes_utf8(self):
        mot = self.songs()[2]
        self.assertEqual(mot['title'], 'Motörhead')
        self.assertEqual(mot['artist'], 'Motörhead')

    def test_written_as_rb3e_writes_them(self):
        self.assertEqual(web_preview.format_songs(self.songs()[:1]),
                         '[rehab]\r\nshortname=rehab\r\ntitle=Rehab\r\nartist=Amy Winehouse\r\n'
                         'album=Back to Black\r\norigin=rb3\r\n\r\n')


class PageTest(unittest.TestCase):
    def test_takes_the_page_out_of_the_header(self):
        header = 'inline constexpr std::string_view kIndexPage = R"html(<p>hi</p>\n)html";\n'
        self.assertEqual(web_preview.index_page(header), '<p>hi</p>\n')


if __name__ == '__main__':
    unittest.main()
