"""Tests for symbolize.py, on a small made-up map and config: python tools/test_symbolize.py"""

import os
import tempfile
import unittest

import symbolize

MAP = """ band3

 Timestamp is 6ac5002d (Tue Oct  6 10:05:33 2026)

 Preferred load address is 0000000140000000

  Address         Publics by Value              Rva+Base               Lib:Object

 0000:00000000       __AbsoluteZero             0000000000000000     <absolute>
 0001:00000000       __imp__App_dt              0000000140001000 f   band3_recomp.0.cpp.obj
 0001:00000100       ?OnPostSetup@Band3App@@AEAAXXZ 0000000140001100 f   main.cpp.obj
 0001:00000400       rex_sub_82279730           0000000140001400 f   band3_recomp.0.cpp.obj
"""

CONFIG = """[functions]
0x82270000 = { name = "App_dt", size = 0x14 }
0x82270018 = { name = "App__DrawRegular", size = 0x68 }
"""


class MapTest(unittest.TestCase):
    def test_symbols_by_rva(self):
        m = symbolize.MapFile("band3.map", MAP)
        self.assertEqual(m.timestamp, "6ac5002d")
        self.assertEqual(m.lookup(0x1000), ("__imp__App_dt", 0))
        self.assertEqual(m.lookup(0x1150), ("?OnPostSetup@Band3App@@AEAAXXZ", 0x50))
        self.assertEqual(m.lookup(0x9999), ("rex_sub_82279730", 0x9999 - 0x1400))
        self.assertIsNone(m.lookup(0x10))

    def test_recompiled_names_lose_their_import_prefix(self):
        self.assertEqual(symbolize.describe(("__imp__App_dt", 4)), "App_dt+0x4")


class GuestTest(unittest.TestCase):
    def test_inside_a_function_only(self):
        g = symbolize.GuestFunctions("band3_config.toml", CONFIG)
        self.assertEqual(g.lookup(0x82270020), ("App__DrawRegular", 8))
        self.assertEqual(g.lookup(0x82270000), ("App_dt", 0))
        self.assertIsNone(g.lookup(0x82270014))  # past App_dt's end, before the next
        self.assertIsNone(g.lookup(0x40001234))  # data


class SymbolizerTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.map_path = os.path.join(self.dir.name, "band3.map")
        with open(self.map_path, "w") as f:
            f.write(MAP)
        self.warnings = []
        self.sym = symbolize.Symbolizer(
            [self.map_path], symbolize.GuestFunctions("c", CONFIG), lambda n: n,
            self.warnings.append)

    def tearDown(self):
        self.dir.cleanup()

    def test_a_report_with_its_build_header(self):
        lines = [
            "=== band3 v1.2 | band3.exe 6ac5002d | started 2026-10-06 10:05:33 UTC | pid 1 ===",
            "[crash-trace] abort() on thread 7 at 2026-10-06 10:07:00 UTC",
            "  #00 ucrtbase.dll+0x71881",
            "  #01 band3.exe+0x1104",
            "  #02 guest 0x82270020 (band3.exe+0x1404)",
        ]
        out = list(self.sym.run(lines))
        self.assertEqual(out[2], "  #00 ucrtbase.dll+0x71881")
        self.assertEqual(out[3], "  #01 band3.exe+0x1104 [?OnPostSetup@Band3App@@AEAAXXZ+0x4]")
        # a guest frame is named once, by its guest function
        self.assertEqual(out[4], "  #02 guest 0x82270020 [App__DrawRegular+0x8] (band3.exe+0x1404)")
        self.assertEqual(self.warnings, [])

    def test_a_header_whose_map_isnt_there_leaves_frames(self):
        lines = [
            "=== band3 v1.1 | band3.exe 12345678 | started 2026-10-06 10:05:33 UTC | pid 1 ===",
            "  #01 band3.exe+0x1104",
        ]
        self.assertEqual(list(self.sym.run(lines))[1], "  #01 band3.exe+0x1104")
        self.assertTrue(any("12345678" in w for w in self.warnings))

    def test_text_without_a_header_uses_the_fallback_map_and_says_so(self):
        out = list(self.sym.run(["  #01 band3.exe+0x1000"]))
        self.assertEqual(out[0], "  #01 band3.exe+0x1000 [App_dt+0x0]")
        self.assertEqual(len(self.warnings), 1)


if __name__ == "__main__":
    unittest.main()
