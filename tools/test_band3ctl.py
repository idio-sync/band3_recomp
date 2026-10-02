"""Tests for band3ctl.py's script handling and window helpers: python tools/test_band3ctl.py"""

import json
import os
import struct
import unittest
import zlib

import band3ctl


class ParseScriptTest(unittest.TestCase):
    def test_keeps_commands_with_their_line_numbers(self):
        script = "instrument guitar\n\npress green\n"
        self.assertEqual(band3ctl.parse_script(script),
                         [(1, "instrument guitar"), (3, "press green")])

    def test_drops_comments_and_blank_lines(self):
        script = "# boot\n   \nwait screen~main timeout=90s   # the main menu\n\t# end\n"
        self.assertEqual(band3ctl.parse_script(script),
                         [(3, "wait screen~main timeout=90s")])

    def test_a_hash_inside_a_word_is_not_a_comment(self):
        self.assertEqual(band3ctl.parse_script("set username band#1\n"),
                         [(1, "set username band#1")])

    def test_windows_line_endings(self):
        self.assertEqual(band3ctl.parse_script("state\r\nquit\r\n"),
                         [(1, "state"), (2, "quit")])


class SplitCommandsTest(unittest.TestCase):
    def test_one_command(self):
        self.assertEqual(band3ctl.split_commands(["press", "green"]), ["press green"])

    def test_semicolons_separate_commands(self):
        self.assertEqual(
            band3ctl.split_commands(["hold", "down;", "wait", "frames=90", ";release", "all"]),
            ["hold down", "wait frames=90", "release all"])

    def test_empty_pieces_are_dropped(self):
        self.assertEqual(band3ctl.split_commands(["state;;", ";"]), ["state"])


class FakeConnection:
    """Replies ok to everything except the commands in `failing`."""

    def __init__(self, failing=()):
        self.sent = []
        self.failing = set(failing)

    def command(self, line):
        self.sent.append(line)
        if line in self.failing:
            return {"ok": False, "error": "timed out", "state": {"screen": "x"}}
        return {"ok": True}


class RunScriptTest(unittest.TestCase):
    def test_runs_every_command_and_passes(self):
        conn = FakeConnection()
        result = band3ctl.run_script(conn, [(1, "state"), (2, "press a")], "boot")
        self.assertTrue(result.passed)
        self.assertEqual(conn.sent, ["state", "press a"])

    def test_stops_at_the_first_failure_and_screenshots_it(self):
        conn = FakeConnection(failing={"wait in_game"})
        result = band3ctl.run_script(
            conn, [(1, "state"), (4, "wait in_game"), (5, "quit")], "boot")
        self.assertFalse(result.passed)
        self.assertEqual(result.line, 4)
        self.assertEqual(result.reply["error"], "timed out")
        self.assertEqual(conn.sent, ["state", "wait in_game", "screenshot fail-boot-line4"])

    def test_replies_with_stats_are_reported(self):
        class StatsConnection(FakeConnection):
            def command(self, line):
                reply = super().command(line)
                if line == "native_view stats":
                    reply["stats"] = {"rendered": 3}
                return reply

        reported = []
        result = band3ctl.run_script(
            StatsConnection(), [(1, "state"), (2, "native_view stats")], "live",
            lambda number, command, reply: reported.append((number, command, reply["stats"])))
        self.assertTrue(result.passed)
        self.assertEqual(reported, [(2, "native_view stats", {"rendered": 3})])

    def test_failure_screenshot_names_are_plain(self):
        conn = FakeConnection(failing={"state"})
        band3ctl.run_script(conn, [(2, "state")], "my script.v2")
        self.assertEqual(conn.sent[-1], "screenshot fail-my_script_v2-line2")


class ReplyTest(unittest.TestCase):
    def test_parses_one_json_line(self):
        self.assertEqual(band3ctl.parse_reply(b'{"ok":true,"state":{"frame":3}}\n'),
                         {"ok": True, "state": {"frame": 3}})

    def test_a_reply_that_isnt_json_is_a_failure(self):
        reply = band3ctl.parse_reply(b"garbage\n")
        self.assertFalse(reply["ok"])
        self.assertIn("garbage", reply["error"])


class WindowHelpersTest(unittest.TestCase):
    def test_parses_a_size(self):
        self.assertEqual(band3ctl.parse_size("1920x1080"), (1920, 1080))
        self.assertEqual(band3ctl.parse_size(" 640X360 "), (640, 360))

    def test_rejects_what_isnt_a_size(self):
        for text in ("1920", "0x720", "axb", "1920x1080x2"):
            with self.assertRaises(ValueError):
                band3ctl.parse_size(text)

    def test_offscreen_is_right_of_the_virtual_screen(self):
        # two monitors, the second left of the primary
        self.assertEqual(band3ctl.offscreen_origin((-1920, 0, 3840, 1080)), (2020, 0))

    def test_only_this_checkouts_build_counts(self):
        repo = os.path.join("C:" + os.sep, "src", "band3")
        build = os.path.join(repo, "out", "build", "win-amd64-release", "band3.exe")
        worktree = os.path.join(repo, ".claude", "worktrees", "x", "out", "build",
                                "win-amd64-release", "band3.exe")
        self.assertTrue(band3ctl.is_build_exe(build, repo))
        if os.name == "nt":  # where paths ignore case
            self.assertTrue(band3ctl.is_build_exe(build.upper(), repo))
        self.assertFalse(band3ctl.is_build_exe(worktree, repo))
        self.assertFalse(band3ctl.is_build_exe(
            os.path.join(repo, "out", "build", "x", "other.exe"), repo))

    def test_png_holds_the_rows(self):
        rows = [bytes([255, 0, 0, 0, 255, 0]), bytes([0, 0, 255, 9, 9, 9])]
        data = band3ctl.png_bytes(2, 2, rows)
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack(">II", data[16:24]), (2, 2))
        length = struct.unpack(">I", data[33:37])[0]
        self.assertEqual(data[37:41], b"IDAT")
        self.assertEqual(zlib.decompress(data[41:41 + length]),
                         b"\x00" + rows[0] + b"\x00" + rows[1])


if __name__ == "__main__":
    unittest.main()
