#!/usr/bin/env python3
"""Archived measurement validation retains its original statistical protocol."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from historical_benchmarks import posterior_gate, speed_gate, MINIMUM_SPEEDUPS


class HistoricalBenchmarkTests(unittest.TestCase):
    def test_shifted_sampler_mean_fails(self):
        runs = [[{"theta": (i % 2) * 0.01} for i in range(100)] for _ in range(3)]
        shifted = [[{"theta": r["theta"] + 1} for r in rows] for rows in runs]
        same = posterior_gate({"stanli": runs, "cmdstan": runs}, ["theta"])
        bad = posterior_gate({"stanli": shifted, "cmdstan": runs}, ["theta"])
        self.assertTrue(same["theta"]["pass"])
        self.assertFalse(bad["theta"]["pass"])

    def test_performance_boundary_and_per_model_failure(self):
        self.assertTrue(speed_gate({"stanli": [2, 2, 2], "cmdstan": [1, 1, 1]})["pass"])
        self.assertFalse(speed_gate({"stanli": [2.01] * 3, "cmdstan": [1] * 3})["pass"])

    def test_pareto_requires_cmdstan_parity(self):
        minimum = MINIMUM_SPEEDUPS["aalto_gpareto"]
        self.assertEqual(minimum, 1.0)
        self.assertTrue(speed_gate({"stanli": [1] * 3, "cmdstan": [1] * 3}, minimum)["pass"])
        self.assertFalse(speed_gate({"stanli": [1.1] * 3, "cmdstan": [1] * 3}, minimum)["pass"])

    def test_missing_or_nonfinite_timings_fail(self):
        for bad in ([], [1, 1], [1, 1, float("nan")], [1, 1, 0]):
            with self.assertRaises(ValueError):
                speed_gate({"stanli": bad, "cmdstan": [1] * 3})


if __name__ == "__main__":
    unittest.main()
