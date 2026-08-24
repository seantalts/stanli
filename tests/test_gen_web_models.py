#!/usr/bin/env python3
"""Focused tests for the generated browser model catalog."""

import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from gen_web_models import DRAWS_MIN, draws_for, num


class DrawCountTests(unittest.TestCase):
    def test_missing_sample_time_uses_the_safe_minimum(self):
        rows = [
            {"stanli_sample_s": "", "note": "stanli_sample_timeout"},
            {},
        ]
        for row in rows:
            with self.subTest(row=row):
                sample_s = num(row, "stanli_sample_s")
                self.assertEqual(draws_for(sample_s), DRAWS_MIN)

    def test_measured_sample_time_keeps_the_existing_buckets(self):
        cases = [(5, 1000), (20, 500), (60, 250), (61, DRAWS_MIN)]
        for sample_s, expected in cases:
            with self.subTest(sample_s=sample_s):
                self.assertEqual(draws_for(sample_s), expected)


if __name__ == "__main__":
    unittest.main()
