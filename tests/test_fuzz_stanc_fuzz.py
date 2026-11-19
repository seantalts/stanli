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
OP_ISLAND                       2         1250          416  20.0%            2
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
        self.assertEqual(fuzz.profile_work(PROFILE),
                         {"calls": 9, "elems": 13, "regions": 2})
        self.assertEqual(fuzz.profile_work("OK 1 2\n"),
                         {"calls": 0, "elems": 0, "regions": 0})


class PerfTest(unittest.TestCase):
    def test_slow_compile_needs_both_conditions(self):
        r = record(o1={"seconds": 1.5, "size": 1}, none={"seconds": 0.1,
                                                         "size": 1000})
        self.assertEqual(fuzz.perf_kinds(r), ["slow_compile"])
        r["none"]["seconds"] = 0.2
        self.assertEqual(fuzz.perf_kinds(r), [])
        r = record(o1={"seconds": 0.5, "size": 1}, none={"seconds": 0.01,
                                                         "size": 1000})
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_serial_times_override_parallel_ones(self):
        r = record(o1={"seconds": 1.5, "size": 1}, none={"seconds": 0.1,
                                                         "size": 1000},
                   serial_seconds={"none": 0.1, "o1": 0.3})
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_mir_growth(self):
        r = record(o1={"seconds": 0.01, "size": 9000}, inlined_size=1000)
        self.assertEqual(fuzz.perf_kinds(r), ["mir_growth"])
        r["inlined_size"] = 1200
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_inline_growth(self):
        r = record(inlined_size=50001)
        self.assertEqual(fuzz.perf_kinds(r), ["inline_growth"])
        r["inlined_size"] = 50000
        self.assertEqual(fuzz.perf_kinds(r), [])

    def test_more_work_screen(self):
        def work(none, o1):
            return {"none": dict(zip(("calls", "elems", "regions"), none)),
                    "o1": dict(zip(("calls", "elems", "regions"), o1))}
        self.assertTrue(fuzz.more_work(work((20, 20, 0), (24, 31, 0))))
        self.assertFalse(fuzz.more_work(work((20, 20, 0), (24, 30, 0))))
        self.assertFalse(fuzz.more_work(work((1, 1, 0), (3, 9, 0))),
                         "too few extra elements")
        self.assertTrue(fuzz.more_work(work((1, 1, 0), (3, 2, 2))),
                        "work inside a region does not show as elements")
        self.assertFalse(fuzz.more_work(work((1, 1, 2), (3, 2, 2))))
        self.assertFalse(fuzz.more_work(None))

    def test_slow_gradient_needs_ratio_and_floor(self):
        r = record(gradient_ns={"none": 200.0, "o1": 301.0})
        self.assertEqual(fuzz.perf_kinds(r), ["slow_gradient"])
        r["gradient_ns"]["o1"] = 300.0
        self.assertEqual(fuzz.perf_kinds(r), [])
        r["gradient_ns"] = {"none": 15.0, "o1": 100.0}
        self.assertEqual(fuzz.perf_kinds(r), [], "under 100 ns more")
        r["gradient_ns"] = {"none": None, "o1": 1000.0}
        self.assertEqual(fuzz.perf_kinds(r), [])
        r = record(work={"none": {"calls": 1, "elems": 1, "regions": 0},
                         "o1": {"calls": 30, "elems": 90, "regions": 0}})
        self.assertEqual(fuzz.perf_kinds(r), [],
                         "the profile alone is not a finding")


class RetimeLimitTest(unittest.TestCase):
    def test_zero_seconds_times_nothing(self):
        slow = record(seed=7, o1={"seconds": 1.5, "size": 1},
                      none={"seconds": 0.1, "size": 1000})
        results = [record(seed=6), slow]
        self.assertEqual(fuzz.retime(None, None, results, 0), 1)
        self.assertNotIn("serial_seconds", slow)
        self.assertEqual(fuzz.retime(None, None, [record()], 0), 0)


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
            record(seed=6, outcome="not_comparable",
                   not_comparable="stanli_refuses_none",
                   refusal="runtime-control region: x7 is not known"),
        ]
        out = io.StringIO()
        fuzz.summarize(results, {"seconds": 2.0, "compiler": "c"}, out)
        text = out.getvalue()
        self.assertIn("6 programs in 2.0 s (3.0 per second)", text)
        self.assertIn("generator rejects: 1", text)
        self.assertIn("different MIR at none and o1: 4 of 5", text)
        self.assertRegex(text, r"stanli refusals\n\s+1\s+runtime-control "
                         r"region: V is not known")
        self.assertIn("dead_code_elimination:write_array", text)
        self.assertRegex(text, r"known buckets\n\s+o1:mir_growth\s+1")
        self.assertRegex(text, r"t_real\s+6")

    def test_reject_message_skips_warnings(self):
        text = ("Warning: Assignment of variable to itself.\n  1 | x = x;\n"
                "Semantic error in 'm.stan', line 3:\n   -------\n"
                "Identifier 'q' not in scope.\n")
        self.assertEqual(fuzz.error_line(text),
                         "Identifier '_' not in scope.")


if __name__ == "__main__":
    unittest.main()
