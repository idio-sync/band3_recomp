"""Tests for band3ctl.py's script handling: python tools/test_band3ctl.py"""

import json
import unittest

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


if __name__ == "__main__":
    unittest.main()
