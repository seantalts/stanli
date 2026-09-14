#!/usr/bin/env python3
"""The educational gate must fail closed on missing or corrupt evidence."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from check_educational import (compare, inventory, parse_timings,
                               posterior_gate, sample_csv, speed_gate, MINIMUM_SPEEDUPS)


class EducationalGateTests(unittest.TestCase):
    def test_shifted_sampler_mean_fails(self):
        runs = [[{"theta": (i % 2) * 0.01} for i in range(100)] for _ in range(3)]
        shifted = [[{"theta": r["theta"] + 1} for r in rows] for rows in runs]
        same = posterior_gate({"stanli": runs, "cmdstan": runs}, ["theta"])
        bad = posterior_gate({"stanli": shifted, "cmdstan": runs}, ["theta"])
        self.assertTrue(same["theta"]["pass"])
        self.assertFalse(bad["theta"]["pass"])

    def test_missing_or_invalid_phase_timings_fail(self):
        for text in ("", "stanli_run: timings prep_s=nan sample_s=1 output_s=1"):
            with self.assertRaises(ValueError):
                parse_timings(text)

    def test_inventory_and_import_hashes(self):
        self.assertEqual(len(inventory()), 13)

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

    def test_generated_quantity_mismatch_fails(self):
        want = {"names": ["yrep"], "lp_grad": [0, 1], "values": [1]}
        self.assertEqual(compare(want, want), 0)
        for got in ({**want, "values": [2]}, {**want, "values": [float("nan")]},
                    {**want, "names": ["wrong"]}, {**want, "lp_grad": [0]}):
            with self.assertRaises(ValueError):
                compare(want, got)

    def test_csv_truncation_nonfinite_and_missing_outputs_fail(self):
        good = "# comment\nlp__,theta,yrep\n-1,0.2,4\n-2,0.3,5\n"
        self.assertEqual(len(sample_csv(good, ["theta", "yrep"], 2)), 2)
        for text in (good.rsplit("-2", 1)[0], good.replace(",5", ",nan"),
                     good.replace("yrep", "wrong"), good.replace("0.3,5", "0.3")):
            with self.assertRaises(ValueError):
                sample_csv(text, ["theta", "yrep"], 2)


if __name__ == "__main__":
    unittest.main()
