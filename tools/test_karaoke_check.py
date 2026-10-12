"""Checks tools/karaoke_check.py's own logic, without a game.
  python tools/test_karaoke_check.py"""
import io
import os
import sys
import unittest
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import karaoke_check


class Reply:
    def __init__(self, status, kind):
        self.status, self.headers = status, {'Content-Type': kind}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def opener(*outcomes):
    """A urlopen stand-in giving each outcome in turn: a status (an HTTPError
    unless 200) or an exception; and the calls made."""
    calls = []

    def urlopen(url, timeout=None):
        calls.append(url)
        out = outcomes[len(calls) - 1]
        if isinstance(out, Exception):
            raise out
        if out != 200:
            raise urllib.error.HTTPError(url, out, 'x', {}, io.BytesIO())
        return Reply(200, 'image/png')
    return urlopen, calls


class FetchTest(unittest.TestCase):
    def test_a_busy_game_is_asked_again_sooner_at_first(self):
        urlopen, calls = opener(503, 503, 200)
        waits = []
        self.assertEqual(karaoke_check.fetch_status('u', opener=urlopen, sleep=waits.append),
                         (200, 'image/png'))
        self.assertEqual(len(calls), 3)
        self.assertEqual(waits, [1, 2])

    def test_a_missing_asset_is_not_asked_again(self):
        urlopen, calls = opener(404)
        waits = []
        self.assertEqual(karaoke_check.fetch_status('u', opener=urlopen, sleep=waits.append), (404, ''))
        self.assertEqual((len(calls), waits), (1, []))

    def test_it_gives_up_after_its_tries(self):
        urlopen, calls = opener(*[503] * 6)
        waits = []
        self.assertEqual(karaoke_check.fetch_status('u', attempts=6, opener=urlopen, sleep=waits.append),
                         (503, ''))
        self.assertEqual(len(calls), 6)
        self.assertEqual(waits, [1, 2, 4, 8, 10])


if __name__ == '__main__':
    unittest.main()
