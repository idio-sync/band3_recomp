"""Tests for band3ctl.py's scripts, --config and window helpers: python tools/test_band3ctl.py"""

import argparse
import json
import os
import shutil
import struct
import tempfile
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


class PlaceConfigTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.exe_folder = os.path.join(self.dir, "build")
        os.makedirs(self.exe_folder)
        self.source = os.path.join(self.dir, "test.toml")
        write(self.source, "lang = \"eng\"\n")
        self.target = os.path.join(self.exe_folder, "band3.toml")

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_the_file_is_there_until_restored_then_gone(self):
        restore = band3ctl.place_config(self.source, self.exe_folder)
        self.assertEqual(read(self.target), "lang = \"eng\"\n")
        restore()
        self.assertEqual(os.listdir(self.exe_folder), [])

    def test_the_players_file_is_kept_aside_and_put_back(self):
        write(self.target, "fullscreen = false\n")
        restore = band3ctl.place_config(self.source, self.exe_folder)
        self.assertEqual(read(self.target), "lang = \"eng\"\n")
        self.assertEqual(read(self.target + ".band3ctl"), "fullscreen = false\n")
        restore()
        self.assertEqual(os.listdir(self.exe_folder), ["band3.toml"])
        self.assertEqual(read(self.target), "fullscreen = false\n")

    def test_a_file_left_aside_before_stops_it(self):
        write(self.target + ".band3ctl", "fullscreen = false\n")
        with self.assertRaises(RuntimeError):
            band3ctl.place_config(self.source, self.exe_folder)
        self.assertFalse(os.path.exists(self.target))

    def test_a_missing_source_leaves_the_players_file(self):
        write(self.target, "fullscreen = false\n")
        with self.assertRaises(OSError):
            band3ctl.place_config(os.path.join(self.dir, "none.toml"), self.exe_folder)
        self.assertEqual(os.listdir(self.exe_folder), ["band3.toml"])
        self.assertEqual(read(self.target), "fullscreen = false\n")


class ConfigProblemTest(unittest.TestCase):
    def test_config_with_the_harness_is_fine(self):
        self.assertIsNone(band3ctl.config_problem(launch_args(config="x.toml")))
        self.assertIsNone(band3ctl.config_problem(launch_args(no_harness=True)))

    def test_config_without_the_harness_is_refused(self):
        problem = band3ctl.config_problem(launch_args(config="x.toml", no_harness=True))
        self.assertIn("--no-harness", problem)

    def test_config_with_the_launcher_is_refused(self):
        problem = band3ctl.config_problem(launch_args(config="x.toml", extra=["--launcher"]))
        self.assertIn("--launcher", problem)
        self.assertIsNone(band3ctl.config_problem(
            launch_args(config="x.toml", extra=["--launcher=false"])))

    def test_launch_stops_before_touching_band3_toml(self):
        folder = tempfile.mkdtemp()
        try:
            exe = os.path.join(folder, "band3.exe")
            write(exe, "")
            target = os.path.join(folder, "band3.toml")
            write(target, PLAYERS_FILE)
            source = os.path.join(folder, "test.toml")
            write(source, "")
            with self.assertRaises(SystemExit) as stopped:
                band3ctl.launch(launch_args(exe=exe, config=source, no_harness=True))
            self.assertIn("--no-harness", str(stopped.exception))
            self.assertEqual(read(target), PLAYERS_FILE)
            self.assertFalse(os.path.exists(target + ".band3ctl"))
        finally:
            shutil.rmtree(folder)


class AsksForLauncherTest(unittest.TestCase):
    def test_the_flag_alone_or_true(self):
        self.assertTrue(band3ctl.asks_for_launcher(["--launcher"]))
        self.assertTrue(band3ctl.asks_for_launcher(["--fast_start=true", "--launcher=true"]))
        self.assertTrue(band3ctl.asks_for_launcher(["--launcher=1"]))

    def test_false_or_absent(self):
        self.assertFalse(band3ctl.asks_for_launcher([]))
        self.assertFalse(band3ctl.asks_for_launcher(["--launcher=false"]))
        self.assertFalse(band3ctl.asks_for_launcher(["--launcher_x"]))

    def test_the_last_one_wins(self):
        self.assertFalse(band3ctl.asks_for_launcher(["--launcher", "--launcher=false"]))
        self.assertTrue(band3ctl.asks_for_launcher(["--launcher=false", "--launcher"]))


PLAYERS_FILE = "fullscreen = false"


def launch_args(exe="band3.exe", config=None, no_harness=False, extra=None):
    """What `launch` reads before it starts anything."""
    return argparse.Namespace(exe=exe, config=config, no_harness=no_harness,
                              extra=extra or [])


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


if __name__ == "__main__":
    unittest.main()
