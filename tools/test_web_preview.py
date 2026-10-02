"""Tests for web_preview.py's song list and album art: python tools/test_web_preview.py"""

import json
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
                ['song', ['name', 'songs/rehab/rehab'], ['vocal_parts', 3]],
                ['game_origin', 'rb3'], ['genre', 'rbsoulfunk'], ['year_released', 2006],
                ['song_length', 214369],
                ['rank', ['drum', 310], ['guitar', 104], ['bass', 138], ['vocals', 241],
                 ['keys', 0], ['band', 255]]],
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
            lambda f: f == 'songs/rehab/gen/rehab_keep.png_xbox',
            {'rbsoulfunk': 'R&B/Soul/Funk'})

    def test_details_as_band3_reports_them(self):
        rehab = self.songs()[0]['details']
        self.assertEqual((rehab['genre'], rehab['year'], rehab['length_ms'], rehab['vocal_parts']),
                         ('R&B/Soul/Funk', 2006, 214369, 3))
        # a rank of 0 is a part the song doesn't have
        self.assertEqual(rehab['tiers'],
                         {'band': 3, 'guitar': 0, 'bass': 1, 'drum': 4, 'vocals': 3})

    def test_details_of_a_song_with_little_in_its_entry(self):
        noart = self.songs()[1]['details']
        self.assertEqual(noart, {'genre': '', 'year': 0, 'length_ms': 0, 'vocal_parts': 0,
                                 'tiers': {}})

    def test_updates_replace_the_fields_they_name(self):
        # Rock Band 3 Deluxe's dx/song_updates/gen/songs_updates.dtb
        updates = [['rehab', ['genre', 'grunge'], ['year_released', 2007]]]
        songs = web_preview.songs_from_dtb(SONGS, lambda d: True, lambda f: False,
                                           {'grunge': 'Grunge'}, updates)
        rehab = songs[0]['details']
        self.assertEqual((rehab['genre'], rehab['year'], rehab['length_ms']),
                         ('Grunge', 2007, 214369))

    def test_details_json_by_shortname(self):
        details = json.loads(web_preview.format_details(self.songs()))
        self.assertEqual(list(details), ['rehab', 'noart', 'mot'])
        self.assertEqual(details['rehab']['tiers']['drum'], 4)

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


class TierTest(unittest.TestCase):
    # the game's thresholds for guitar, as SongMgr::RankTier has them
    def test_the_first_threshold_the_rank_is_under(self):
        self.assertEqual(web_preview.tier('guitar', 1), 0)
        self.assertEqual(web_preview.tier('guitar', 138), 0)
        self.assertEqual(web_preview.tier('guitar', 139), 1)
        self.assertEqual(web_preview.tier('guitar', 474), 6)

    def test_past_the_last_threshold_is_impossible(self):
        self.assertEqual(web_preview.tier('guitar', 900), 6)


class StatusTest(unittest.TestCase):
    SONG = {'shortname': 'rehab', 'title': 'Rehab', 'artist': 'Amy Winehouse',
            'details': {'length_ms': 200000}}

    def test_menu_and_library(self):
        self.assertEqual(web_preview.demo_status('menu', [self.SONG], 0),
                         {'screen': 'main_hub_screen', 'in_library': False, 'playing': None})
        self.assertTrue(web_preview.demo_status('library', [self.SONG], 0)['in_library'])

    def test_playing_goes_round_the_song(self):
        playing = web_preview.demo_status('playing', [self.SONG], 230)['playing']
        self.assertEqual(playing['shortname'], 'rehab')
        self.assertEqual(playing['length_ms'], 200000)
        self.assertEqual(playing['position_ms'], 30000)
        self.assertGreater(playing['score'], 0)


class PageTest(unittest.TestCase):
    def test_takes_the_page_out_of_the_header(self):
        header = 'inline constexpr std::string_view kIndexPage = R"html(<p>hi</p>\n)html";\n'
        self.assertEqual(web_preview.index_page(header), '<p>hi</p>\n')


class RhythmVerseTest(unittest.TestCase):
    """As band3's tests/rhythmverse_test.cpp checks the same conversion."""

    REPLY = {'status': 'success', 'data': {
        'records': {'total_filtered': 812}, 'pagination': {'page': '2'},
        'songs': [
            {'data': {'artist': 'Rob Zombie', 'title': 'Never Gonna Stop', 'diff_keys': '0',
                      'album_art': '/assets/album_art/n/x.png'},
             'file': {'file_id': '595481a7cbc158.68319817', 'file_title': 'Never Gonna Stop (Red)',
                      'song_length': 190, 'size': 3706880, 'zippata': 0, 'external_url': '',
                      'diff_drums': 4, 'diff_vocals': 1, 'diff_proguitar': -1,
                      'author': {'name': 'DenVaktare'}, 'file_url': '/songfile/595481a7cbc158.68319817',
                      'download_url': '/download_file/denvaktare/595481a7cbc158.68319817/DV'}},
            {'data': {'artist': 'Band', 'title': 'Elsewhere'},
             'file': {'file_id': '5b94.1', 'external_url': 'https://www.mediafire.com/f/x.rar',
                      'author': None, 'user': 'someone'}},
            {'data': {}, 'file': {'file_id': '60aa', 'zippata': 1, 'download_url': '/download_file/u/z.zip'}},
            {'data': {}, 'file': {'file_id': '../evil'}},
        ]}}

    def test_songs_as_band3_gives_them(self):
        result = web_preview.rv_search_result(self.REPLY, {'595481a7cbc158.68319817'})
        self.assertEqual((result['total'], result['page'], len(result['songs'])), (812, 2, 3))
        song = result['songs'][0]
        self.assertEqual(song['title'], 'Never Gonna Stop (Red)')
        self.assertEqual(song['artist'], 'Rob Zombie')
        self.assertEqual(song['tiers'], {'drum': 3, 'vocals': 0})
        self.assertEqual(song['length_ms'], 190000)
        self.assertEqual(song['author'], 'DenVaktare')
        self.assertEqual(song['art'], 'https://rhythmverse.co/assets/album_art/n/x.png')
        self.assertEqual(song['page'], 'https://rhythmverse.co/songfile/595481a7cbc158.68319817')
        self.assertTrue(song['download'])
        self.assertTrue(song['downloaded'])

    def test_only_hosted_unzipped_songs_download(self):
        songs = web_preview.rv_search_result(self.REPLY, set())['songs']
        self.assertEqual((songs[1]['download'], songs[1]['host'], songs[1]['author']),
                         (False, 'www.mediafire.com', 'someone'))
        self.assertEqual((songs[2]['download'], songs[2]['host']), (False, 'rhythmverse.co'))

    def test_a_search_that_found_nothing_is_empty(self):
        result = web_preview.rv_search_result(
            {'status': 'success', 'data': {'records': {'total_filtered': 0}, 'songs': False}}, set())
        self.assertEqual((result['total'], result['songs']), (0, []))

    def test_a_failed_reply_is_none(self):
        self.assertIsNone(web_preview.rv_search_result({'status': 'error'}, set()))
        self.assertIsNone(web_preview.rv_search_result([], set()))

    def test_search_or_newest(self):
        url, form, size = web_preview.rv_search_form({'text': 'zombie', 'page': '2'})
        self.assertTrue(url.endswith('/search/live'))
        self.assertIn(('text', 'zombie'), form)
        self.assertIn(('page', 2), form)
        self.assertEqual(size, 25)
        url, form, _ = web_preview.rv_search_form({'page': '0'})
        self.assertTrue(url.endswith('/songfiles/list'))
        self.assertIn(('page', 1), form)
        self.assertIn(('sort[0][sort_by]', 'release_date'), form)

    def test_filters_as_band3_sends_them(self):
        _, form, size = web_preview.rv_search_form({
            'text': 'x', 'sort': 'downloads', 'downloadable': '1', 'has': 'real_keys,banjo',
            'harmonies': '1', 'genre': 'metal,Bad-Genre,m\u00e9tal', 'decade': '1990,1995,0,2150',
            'cap': 'drum:2'})
        self.assertEqual(size, 100)
        self.assertEqual(form[3:], [
            ('text', 'x'), ('sort[0][sort_by]', 'downloads'), ('sort[0][sort_order]', 'DESC'),
            ('instrument[]', 'prokeys'), ('vocal_parts[]', '2'), ('vocal_parts[]', '3'),
            ('genre[]', 'metal'), ('decade[]', '1990'), ('tierinstrument[]', 'drums'),
            ('tier[]', '1'), ('tier[]', '2'), ('tier[]', '3')])


if __name__ == '__main__':
    unittest.main()
