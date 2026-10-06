"""Tests for symbolize.py, on a small made-up map and config: python tools/test_symbolize.py"""

import os
import struct
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
        g = symbolize.GuestFunctions("band3_functions.toml", CONFIG)
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


def tiny_elf(build_id):
    """A 64-bit ELF header, one PT_NOTE program header and a GNU build id note."""
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\x00" + build_id
    phoff, note_at = 64, 64 + 56
    header = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    header += struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, phoff, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 4, 4, note_at, note_at, note_at, len(note), len(note), 4)
    return header + phdr + note


class FakeElf:
    def __init__(self, path):
        self.path = path

    def lookup_many(self, offsets):
        return {o: {0x1234: "Band3App::OnPostSetup()", 0x5678: "App_dt"}.get(o) for o in offsets}


class LinuxTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.elf = os.path.join(self.dir.name, "band3")
        with open(self.elf, "wb") as f:
            f.write(tiny_elf(bytes.fromhex("0a1b2c3d")))
        self.warnings = []

    def tearDown(self):
        self.dir.cleanup()

    def symbolizer(self):
        return symbolize.Symbolizer([], None, lambda n: n, self.warnings.append,
                                    elfs=[self.elf], open_elf=FakeElf)

    def test_reads_the_build_id(self):
        self.assertEqual(symbolize.file_build_id(self.elf), "0a1b2c3d")
        self.assertIsNone(symbolize.elf_build_id(b"MZ\x90\x00"))

    def test_names_frames_from_the_executable_with_the_reports_build_id(self):
        lines = [
            "=== band3 v1.2 | band3 build-id 0a1b2c3d | started 2026-10-06 10:05:33 UTC | pid 1 ===",
            "[crash-trace] segmentation fault (SIGSEGV), nothing mapped at 0x0 on thread 7",
            "  #00 band3+0x1234",
            "  #01 libc.so.6+0x42520",
            "  #02 band3+0x5678",
            "  #03 band3+0x9999",
        ]
        out = self.symbolizer().run(lines)
        self.assertEqual(out[2], "  #00 band3+0x1234 [Band3App::OnPostSetup()]")
        self.assertEqual(out[3], "  #01 libc.so.6+0x42520")
        self.assertEqual(out[4], "  #02 band3+0x5678 [App_dt]")
        self.assertEqual(out[5], "  #03 band3+0x9999")
        self.assertEqual(self.warnings, [])

    def test_another_build_leaves_frames(self):
        lines = ["=== band3 v1.1 | band3 build-id ffff | started x UTC | pid 1 ===",
                 "  #00 band3+0x1234"]
        self.assertEqual(self.symbolizer().run(lines)[1], "  #00 band3+0x1234")
        self.assertTrue(any("ffff" in w for w in self.warnings))

    def test_windows_frames_arent_linux_ones(self):
        self.assertIsNone(symbolize.ELF_FRAME.search("  #01 band3.exe+0x1104"))


if __name__ == "__main__":
    unittest.main()
