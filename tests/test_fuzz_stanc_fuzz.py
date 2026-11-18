#!/usr/bin/env python3
"""Tests for tools/fuzz_stanc/fuzz.py's seed lists, performance buckets,
known-bucket matching and summary. Nothing here runs a compiler."""
from __future__ import annotations

import io
import json
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))

import fuzz  # noqa: E402

PROFILE = """\
opcode                      calls       fwd ns       bwd ns      %        elems
OP_TANHV                        5          833          459  42.5%            5
OP_NORMAL_LPDF                  2          833           83  30.1%            6
                                          3043 ns total
"""


def record(**kw):
    r = {"seed": 1, "outcome": "agree", "features": ["t_real"],
         "mir_identical": False, "points_with_values": 3,
         "none": {"seconds": 0.01, "size": 1000},
         "o1": {"seconds": 0.02, "size": 1500},
         "inlined_size": 1200, "work": None, "buckets": []}
    r.update(kw)
    return r


class SeedsTest(unittest.TestCase):
    def test_ranges_and_lists(self):
        self.assertEqual(fuzz.parse_seeds("3:6"), [3, 4, 5])
        self.assertEqual(fuzz.parse_seeds("1,4:6,9"), [1, 4, 5, 9])
        with self.assertRaises(ValueError):
            fuzz.parse_seeds("6:3")


class ProfileTest(unittest.TestCase):
    def test_totals(self):
        self.assertEqual(fuzz.profile_work(PROFILE), {"calls": 7, "elems": 11})
        self.assertEqual(fuzz.profile_work("OK 1 2\n"),
                         {"calls": 0, "elems": 0})


class PerfTest(unittest.TestCase):
    def test_slow_compile_needs_both_conditions(self):
        r = record(o1={"seconds": 1.5, "size": 1}, none={"seconds": 0.1,
                                                         "size": 1})
        self.assertEqual(fuzz.perf_kinds(r), ["slow_compile"])
        r["none"]["seconds"] = 0.2
        self.assertEqual(fuzz.perf_kinds(r), [])
        r = record(o1={"seconds": 0.5, "size": 1}, none={"seconds": 0.01,
                                                         "size": 1})
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_serial_times_override_parallel_ones(self):
        r = record(o1={"seconds": 1.5, "size": 1}, none={"seconds": 0.1,
                                                         "size": 1},
                   serial_seconds={"none": 0.1, "o1": 0.3})
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_mir_growth(self):
        r = record(o1={"seconds": 0.01, "size": 9000}, inlined_size=1000)
        self.assertEqual(fuzz.perf_kinds(r), ["mir_growth"])
        r["inlined_size"] = 1200
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_more_ad_work(self):
        r = record(work={"none": {"calls": 10, "elems": 10},
                         "o1": {"calls": 12, "elems": 16}})
        self.assertEqual(fuzz.perf_kinds(r), ["more_ad_work"])
        r["work"]["o1"]["elems"] = 15
        self.assertEqual(fuzz.perf_kinds(r), [])
        r["work"] = {"none": {"calls": 0, "elems": 0},
                     "o1": {"calls": 3, "elems": 3}}
        self.assertEqual(fuzz.perf_kinds(r), [])


class KnownTest(unittest.TestCase):
    KNOWN = [{"bucket": "function_inlining:output",
              "match": "pair", "reason": "r"}]

    def test_bucket_and_pattern_must_both_match(self):
        diffs = [{"kind": "output", "none": ["pair"]}]
        self.assertEqual(fuzz.known_entry("function_inlining:output", diffs,
                                          self.KNOWN), self.KNOWN[0])
        self.assertIsNone(fuzz.known_entry("function_inlining:lp", diffs,
                                           self.KNOWN))
        self.assertIsNone(fuzz.known_entry(
            "function_inlining:output", [{"kind": "output", "none": ["x"]}],
            self.KNOWN))

    def test_seed_file_parses(self):
        known = json.loads((REPO / "tools" / "fuzz_stanc" /
                            "known.json").read_text())
        for k in known:
            self.assertEqual(sorted(k), ["bucket", "match", "reason"])


class SummaryTest(unittest.TestCase):
    def test_counts(self):
        reject = record(seed=3, outcome="reject", reject="Semantic error: x")
        del reject["mir_identical"], reject["o1"]
        results = [
            record(seed=1),
            record(seed=2, mir_identical=True),
            reject,
            record(seed=4, outcome="differ",
                   buckets=["dead_code_elimination:write_array"],
                   diffs=[{"kind": "write_array"}]),
            record(seed=5, outcome="differ", buckets=["o1:mir_growth"],
                   known={"o1:mir_growth": "why"}),
        ]
        out = io.StringIO()
        fuzz.summarize(results, {"seconds": 2.0, "compiler": "c"}, out)
        text = out.getvalue()
        self.assertIn("5 programs in 2.0 s (2.5 per second)", text)
        self.assertIn("generator rejects: 1", text)
        self.assertIn("different MIR at none and o1: 3 of 4", text)
        self.assertIn("dead_code_elimination:write_array", text)
        self.assertRegex(text, r"known buckets\n\s+o1:mir_growth\s+1")
        self.assertRegex(text, r"t_real\s+5")


if __name__ == "__main__":
    unittest.main()
