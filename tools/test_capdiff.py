"""Tests for capdiff.py's parsers and comparison: python tools/test_capdiff.py

The --list parser runs on native_view_replay's own output for two captures
(tests/golden/capdiff_list_*.txt, cut after the first few draws): a song's post
frame from out/parity4 (evenodd-25s) and the title from out/n1/f, which has a
render target no pass made.
"""

import copy
import os
import struct
import tempfile
import unittest
import zlib

import capdiff

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN = os.path.join(REPO, "tests", "golden")


def golden(name):
    with open(os.path.join(GOLDEN, name), encoding="utf-8") as f:
        return capdiff.parse_list(f.read())


def write_png(path, width, height, pixel, filter_type=0, alpha=False):
    """A PNG whose pixel (x, y) is pixel(x, y) (an RGB tuple), each row stored
    with filter_type (0 none, 1 sub, 2 up)."""
    bpp = 4 if alpha else 3
    raw, prev = bytearray(), bytes(width * bpp)
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes(pixel(x, y)) + (b"\xff" if alpha else b"")
        if filter_type == 1:
            out = bytes((row[i] - (row[i - bpp] if i >= bpp else 0)) & 255
                        for i in range(len(row)))
        elif filter_type == 2:
            out = bytes((a - b) & 255 for a, b in zip(row, prev))
        else:
            out = bytes(row)
        raw += bytes([filter_type]) + out
        prev = bytes(row)

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2,
                                           0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw))))
        f.write(chunk(b"IEND", b""))


class ParseListTest(unittest.TestCase):
    def test_song_post_frame_counts(self):
        c = golden("capdiff_list_song.txt")
        self.assertEqual((c["frame"], c["draws"], c["back_buffer_meshes"], c["cameras"]),
                         (10, 374, 235, 4))
        self.assertEqual((c["pass_count"], c["texture_passes"], c["passes_carried"]), (31, 28, 3))
        self.assertEqual((c["game_frame"], c["rt_sampled"], c["rt_missing"], c["rt_filtered"]),
                         (3366, 21, 0, 0))
        self.assertEqual(c["skipped"], {"skipped_shadow": 0, "skipped_velocity": 11,
                                        "skipped_draw_mode": 0, "skipped_no_geom": 15,
                                        "skipped_target": 2})
        self.assertEqual((c["post_boundary"], c["proc_cmds"], c["composed"]), (172, 2, True))
        self.assertTrue(c["post"].startswith("boundary 172, proc_cmds 2, proc 2449EED8;"))
        self.assertTrue(c["check"].startswith("flags DOF 1 bloom 1 glare 0 xfm 1"))
        self.assertTrue(c["gamma"].startswith("table; value: shown r/g/b 0:0/0/0"))
        self.assertEqual(c["missing_rts"], [])

    def test_song_pass_list(self):
        passes = golden("capdiff_list_song.txt")["passes"]
        self.assertEqual(len(passes), 31)
        self.assertEqual([p["index"] for p in passes], list(range(31)))
        first = passes[0]
        self.assertEqual((first["target"], first["address"], first["kind"], first["size"],
                          first["mips"], first["draws"], first["version"], first["frame"],
                          first["carried"], first["name"]),
                         ("texture", "209D5798", "rendered-noz", "512x256", 0, 4, 1, 1802, True,
                          "51squier_paint_diff_output.tex"))
        # an unnamed pass with draw modes: the name stops before them
        self.assertEqual((passes[5]["name"], passes[5]["modes"], passes[5]["draws"]),
                         ("-", ["3"], 11))
        self.assertEqual((passes[9]["kind"], passes[9]["size"], passes[9]["modes"]),
                         ("shadow-map", "512x512", ["1"]))
        self.assertEqual((passes[8]["target"], passes[8]["draws"]), ("back buffer", 4))
        self.assertEqual((passes[30]["target"], passes[30]["draws"]), ("back buffer", 95))
        # the passes cover every draw, in order
        self.assertEqual(sum(p["draws"] for p in passes), 374)

    def test_title_missing_target_and_back_buffer_modes(self):
        c = golden("capdiff_list_title.txt")
        self.assertEqual((c["draws"], c["rt_missing"], c["proc_cmds"], c["composed"]),
                         (493, 1, 2, True))
        self.assertEqual(c["missing_rts"],
                         ["no pass for 2636C910 version 61 (rendered-noz), sampled by 2 draws"])
        bb = c["passes"][18]
        self.assertEqual((bb["target"], bb["draws"], bb["modes"]), ("back buffer", 157, ["7"]))
        self.assertEqual(sum(p["draws"] for p in c["passes"]), 493)

    def test_a_frame_of_its_own_isnt_composed(self):
        text = ("frame 2, 4 draws (0 of them meshes to the back buffer), 0 cameras\n"
                "passes: 1 (0 into textures, 0 of them carried from earlier frames); game frame "
                "90; render targets sampled 0, missing 0, their pass's draws all left out 0; "
                "snapshots 0; empty passes left out 0, unbalanced 0\n"
                "composed: no, the world is the frame's own\n"
                "pass   0 back buffer draws 0..4\n")
        c = capdiff.parse_list(text)
        self.assertFalse(c["composed"])
        self.assertEqual(c["passes"], [{"index": 0, "target": "back buffer", "draws": 4,
                                        "modes": []}])

    def test_output_without_a_header_is_an_error(self):
        with self.assertRaises(ValueError):
            capdiff.parse_list("replay: can't read x.cap\n")


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.r = golden("capdiff_list_song.txt")
        self.n = copy.deepcopy(self.r)

    def test_the_same_capture_is_equal(self):
        self.assertEqual(capdiff.compare(self.r, self.n, False), ([], []))

    def test_addresses_and_versions_dont_count(self):
        for p in self.n["passes"]:
            if p["target"] == "texture":
                p.update(address="12345678", version=p["version"] + 7, frame=p["frame"] + 9)
        self.n["post"] = self.n["post"].replace("2449EED8", "2449F000")
        self.assertEqual(capdiff.compare(self.r, self.n, False), ([], []))

    def test_a_missing_pass_fails(self):
        del self.n["passes"][3]
        fails, _ = capdiff.compare(self.r, self.n, False)
        self.assertEqual(len(fails), 1)
        self.assertIn("pass list: 31 vs 30 passes", fails[0])
        self.assertIn("R only: texture rendered-noz 512x256 mips 0: head_wrinkle_output.tex",
                      fails[0])

    def test_another_target_size_fails(self):
        self.n["passes"][11]["size"] = "480x270"
        fails, _ = capdiff.compare(self.r, self.n, False)
        self.assertTrue(fails and fails[0].startswith("pass list"))

    def test_gamma_fails(self):
        self.n["gamma"] = "pwl; value: shown r/g/b 0:0/0/0"
        fails, _ = capdiff.compare(self.r, self.n, False)
        self.assertEqual(len(fails), 1)
        self.assertTrue(fails[0].startswith("gamma: "))

    def test_rt_missing_fails(self):
        self.n["rt_missing"] = 1
        fails, _ = capdiff.compare(self.r, self.n, False)
        self.assertEqual(fails, ["rt_missing 0 vs 1"])

    def test_draws_must_be_equal_on_a_still_screen(self):
        self.n["draws"] += 2
        self.n["passes"][30]["draws"] += 2
        fails, _ = capdiff.compare(self.r, self.n, False)
        self.assertEqual(fails, ["draws 374 vs 376", "draws by pass: pass 30 95 vs 97"])

    def test_a_moving_screen_has_ten_percent(self):
        self.n["draws"] += 30
        self.n["passes"][30]["draws"] += 30
        fails, notes = capdiff.compare(self.r, self.n, True)
        self.assertEqual(fails, [])
        self.assertIn("draws 374 vs 404 (moving: within 10%)", notes)
        self.n["draws"] += 20
        fails, _ = capdiff.compare(self.r, self.n, True)
        self.assertEqual(fails, ["draws 374 vs 424 (more than 10%)"])

    def test_post_and_check_are_notes(self):
        self.n["post"] = self.n["post"].replace("focal 115.31", "focal 115.40")
        self.n["check"] = "something else"
        self.n["skipped"]["skipped_velocity"] = 12
        fails, notes = capdiff.compare(self.r, self.n, False)
        self.assertEqual(fails, [])
        self.assertEqual(notes, ["skipped_velocity 11 vs 12", "post: differs", "check: differs"])

    def test_replies(self):
        r_reply = {"capture": "a.cap", "rt_missing": 0, "rt_filtered": 1, "proc_cmds": 2,
                   "composed": True, "game_frame": 3366, "world_frame": 3365,
                   "skipped_pass": 13, "skipped_shadow": 0, "held_fallback": False}
        n_reply = dict(r_reply, game_frame=4100, world_frame=4099)
        # the frames themselves differ run to run; the world frame's distance doesn't
        self.assertEqual(capdiff.compare(self.r, self.n, False, r_reply, n_reply), ([], []))
        n_reply.update(rt_missing=2, proc_cmds=7, world_frame=4098)
        fails, notes = capdiff.compare(self.r, self.n, False, r_reply, n_reply)
        self.assertEqual(fails, ["reply rt_missing 0 vs 2"])
        self.assertEqual(notes, ["reply proc_cmds 2 vs 7",
                                 "reply world frame 1 vs 2 before the game frame"])


class TextureSetTest(unittest.TestCase):
    def test_title_samples(self):
        c = golden("capdiff_list_title.txt")
        # draw 2 samples norm_output.tex's version 2 (its line has the size);
        # the cut file ends on draw 3's sample, before its draw line
        self.assertEqual(c["rt_samples"], [
            {"address": "20F70D58", "type": 0x22, "version": 2, "size": "512x256"},
            {"address": "20F75448", "type": 0x22, "version": 1, "size": None}])
        self.assertEqual(c["loaded"], ["512x256 fmt 49", "512x256 fmt 49"])
        self.assertEqual(capdiff.texture_sets(c), ({"norm_output.tex"}, {"512x256 fmt 49"}))

    def test_song_samples(self):
        c = golden("capdiff_list_song.txt")
        # draw 0 samples nothing (tex -0x0); 1 and 2 a loaded texture
        self.assertEqual(c["rt_samples"], [])
        self.assertEqual(capdiff.texture_sets(c), (set(), {"512x256 fmt 20"}))

    def test_the_same_capture_is_the_same_content(self):
        c = golden("capdiff_list_title.txt")
        t = capdiff.compare_textures(c, copy.deepcopy(c))
        self.assertTrue(t["same"])
        self.assertEqual(capdiff.texture_text(t), "same content")

    def test_addresses_and_versions_dont_count(self):
        r = golden("capdiff_list_title.txt")
        n = copy.deepcopy(r)
        # another run: norm_output.tex at another address, sampled at another version
        for p in n["passes"]:
            if p["target"] == "texture" and p["address"] == "20F70D58":
                p["address"] = "21000000"
        n["rt_samples"][0].update(address="21000000", version=9)
        self.assertTrue(capdiff.compare_textures(r, n)["same"])

    def test_unnamed_and_passless_targets(self):
        c = golden("capdiff_list_title.txt")
        # the post chain's 320x180 target has no name; 2636C910 has a pass
        # (clouds_rnd.tex) whatever version is sampled; 12345678 has none
        c["rt_samples"] = [
            {"address": "2262A5E8", "type": 0x22, "version": 184, "size": "320x180"},
            {"address": "2636C910", "type": 0x22, "version": 61, "size": "512x512"},
            {"address": "12345678", "type": 0x1000, "version": 0, "size": "64x64"}]
        self.assertEqual(capdiff.texture_sets(c)[0],
                         {"- rendered-noz 320x180", "clouds_rnd.tex",
                          "(no pass) type 0x1000 64x64"})

    def test_differences_are_listed_by_side(self):
        r = golden("capdiff_list_title.txt")
        n = copy.deepcopy(r)
        n["rt_samples"].append({"address": "2636C910", "type": 0x22, "version": 62,
                                "size": "512x512"})
        n["loaded"] = ["256x256 fmt 18"]
        t = capdiff.compare_textures(r, n)
        self.assertFalse(t["same"])
        self.assertEqual(t["rt"], {"r_only": [], "n_only": ["clouds_rnd.tex"]})
        self.assertEqual(t["loaded"], {"r_only": ["512x256 fmt 49"],
                                       "n_only": ["256x256 fmt 18"]})
        self.assertEqual(capdiff.texture_text(t),
                         "content differs: render targets N only: clouds_rnd.tex; loaded "
                         "textures R only: 512x256 fmt 49; loaded textures N only: "
                         "256x256 fmt 18")
        # informative: compare() doesn't fail on it
        self.assertEqual(capdiff.compare(r, n, False), ([], []))


class RunLogTest(unittest.TestCase):
    def test_run_output_and_bare_replies(self):
        log = "\n".join([
            "tests/game/render_song.b3t:70: capture render-song-start: "
            '{"ok": true, "path": "C:\\\\b3\\\\screenshots\\\\render-song-start.png", '
            '"capture": "C:\\\\b3\\\\screenshots\\\\render-song-start.cap", "rt_missing": 0}',
            'tests/game/render_song_live.b3t:9: native_view stats: {"rendered": 3}',
            '{"ok": true, "capture": "screenshots/screen-title.cap", "rt_missing": 1}',
            "tests/game/x.b3t:3: capture broken: {not json",
            "PASS tests/game/render_song.b3t: 60 commands in 95.0 s",
        ])
        replies = capdiff.parse_run_log(log)
        self.assertEqual(sorted(replies), ["render-song-start", "screen-title"])
        self.assertEqual(replies["render-song-start"]["rt_missing"], 0)
        self.assertEqual(replies["screen-title"]["rt_missing"], 1)

    def test_a_later_capture_of_the_same_name_wins(self):
        log = ('{"capture": "s/a.cap", "draws": 1}\n'
               '{"capture": "s/a.cap", "draws": 2}\n')
        self.assertEqual(capdiff.parse_run_log(log)["a"]["draws"], 2)


class PngTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.tmp.name, name)

    def test_reads_every_filter_and_alpha(self):
        pixel = lambda x, y: ((x * 7 + y) & 255, (y * 13) & 255, (x * y) & 255)
        want = [bytes(v for x in range(5) for v in pixel(x, y)) for y in range(4)]
        for filter_type in (0, 1, 2):
            for alpha in (False, True):
                write_png(self.path("p.png"), 5, 4, pixel, filter_type, alpha)
                self.assertEqual(capdiff.read_png(self.path("p.png")), (5, 4, want),
                                 (filter_type, alpha))

    def test_motion_is_the_worst_block(self):
        still = lambda x, y: (100, 100, 100)
        # one 32x32 block 30 brighter in every channel, and a pixel elsewhere
        moved = lambda x, y: (130, 130, 130) if 32 <= x < 64 and y < 32 else \
            (200, 100, 100) if (x, y) == (5, 40) else (100, 100, 100)
        write_png(self.path("a.png"), 96, 64, still)
        write_png(self.path("b.png"), 96, 64, moved, 1)
        self.assertEqual(capdiff.motion(self.path("a.png"), self.path("a.png")), 0)
        self.assertAlmostEqual(capdiff.motion(self.path("a.png"), self.path("b.png")), 30)
        write_png(self.path("c.png"), 64, 64, still)
        self.assertIsNone(capdiff.motion(self.path("a.png"), self.path("c.png")))

    def test_classify(self):
        write_png(self.path("s.png"), 64, 64, lambda x, y: (0, 0, 0))
        write_png(self.path("s-again.png"), 64, 64, lambda x, y: (1, 1, 1))
        write_png(self.path("m.png"), 64, 64, lambda x, y: (0, 0, 0))
        write_png(self.path("m-again.png"), 64, 64, lambda x, y: (9, 9, 9))
        write_png(self.path("lone.png"), 64, 64, lambda x, y: (0, 0, 0))
        self.assertEqual(capdiff.classify(self.tmp.name, "s", set(), 2.0)[0], "still")
        self.assertEqual(capdiff.classify(self.tmp.name, "m", set(), 2.0)[0], "moving")
        self.assertEqual(capdiff.classify(self.tmp.name, "lone", set(), 2.0),
                         ("still", "no -again shot"))
        self.assertEqual(capdiff.classify(self.tmp.name, "lone", {"lone"}, 2.0),
                         ("moving", "--moving"))


if __name__ == "__main__":
    unittest.main()
