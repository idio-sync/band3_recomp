"""Tests for perf_sample.py's parts that need no process: python tools/test_perf_sample.py"""

import os
import tempfile
import unittest

import perf_sample


class InstanceTest(unittest.TestCase):
    def test_the_process_and_engine(self):
        name = "pid_1234_luid_0x00000000_0x0000D1B5_phys_0_eng_0_engtype_3D"
        self.assertTrue(perf_sample.instance_matches(name, 1234, "3D"))
        self.assertTrue(perf_sample.instance_matches(name, 1234))
        self.assertFalse(perf_sample.instance_matches(name, 1234, "Copy"))
        # pid 123 isn't pid 1234's prefix
        self.assertFalse(perf_sample.instance_matches(name, 123, "3D"))
        self.assertTrue(perf_sample.instance_matches(
            "pid_1234_luid_0x00000000_0x0000D1B5_phys_0", 1234))


class GuardTest(unittest.TestCase):
    def test_trips_after_the_time_over_straight(self):
        g = perf_sample.Guard(95, 5)
        self.assertEqual([g.check(t, 99) for t in range(1, 7)],
                         [False, False, False, False, False, True])

    def test_a_dip_starts_over(self):
        g = perf_sample.Guard(95, 5)
        values = [99, 99, 99, 50, 99, 99, 99, 99, 99, 99]
        self.assertEqual([g.check(t, v) for t, v in enumerate(values, 1)],
                         [False] * 9 + [True])

    def test_no_counter_never_trips(self):
        g = perf_sample.Guard(95, 1)
        self.assertFalse(any(g.check(t, None) for t in range(10)))


class SummaryTest(unittest.TestCase):
    def test_means_peaks_and_threads(self):
        samples = [{"cpu_pct": 10, "cpu_cores": 1.6, "gpu3d_pct": 20, "vram_mb": 700},
                   {"cpu_pct": 30, "cpu_cores": 4.8, "gpu3d_pct": None, "vram_mb": 710}]
        s = perf_sample.summarize(samples, {1: 100.0, 2: 50.0, 3: 5.0}, {1: 400.0, 2: 60.0, 4: 7.0},
                                  {1: "game"}, {2: "sync gpu: ring at 1FC9D000"})
        self.assertEqual(s["samples"], 2)
        self.assertEqual(s["cpu_pct"], {"mean": 20, "peak": 30})
        self.assertEqual(s["gpu3d_pct"], {"mean": 20, "peak": 20})
        self.assertEqual(s["vram_mb"], {"mean": 705, "peak": 710})
        self.assertEqual(s["threads"], [
            {"tid": 1, "cpu_ms": 300.0, "name": "game"},
            {"tid": 2, "cpu_ms": 10.0, "log": "sync gpu: ring at 1FC9D000"},
            {"tid": 4, "cpu_ms": 7.0, "started": True},
            {"tid": 3, "cpu_ms": 0.0, "ended": True},
        ])

    def test_no_samples(self):
        s = perf_sample.summarize([], {}, {})
        self.assertEqual((s["samples"], s["cpu_pct"], s["threads"]), (0, None, []))


class LogTagsTest(unittest.TestCase):
    def test_first_line_per_thread(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "band3_001.log")
            with open(path, "w", encoding="utf-8") as f:
                f.write("[2026-10-05 12:32:35.313] [info] [core] [t10656] band3 build x\n"
                        "[2026-10-05 12:32:35.400] [info] [core] [t2912] sync gpu: started\n"
                        "[2026-10-05 12:32:36.000] [info] [core] [t10656] later\n"
                        "no tag here\n")
            self.assertEqual(perf_sample.read_log_tags(path),
                             {10656: "band3 build x", 2912: "sync gpu: started"})


if __name__ == "__main__":
    unittest.main()
