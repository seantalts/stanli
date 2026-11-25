#!/usr/bin/env python3
"""Tests for tools/fuzz_stanc/differential.py's comparison and buckets.

Synthetic stanli_check output stands in for real runs, so nothing here
needs a build or the stanc3 checkout.
"""
from __future__ import annotations

import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))
sys.path.insert(0, str(REPO / "tools"))

import check_status  # noqa: E402
import differential as d  # noqa: E402

OK_STATUS = check_status.CheckStatus(
    {"record": "compile", "ok": True, "interpreter_fallbacks": [],
     "write_array": {"section": "graph", "columns": 2, "truncated": None}})


def output(points, pre=()):
    """stanli_check --points output: {k: [lines]}."""
    lines = list(pre)
    for k, body in sorted(points.items()):
        lines.append(f"POINT {k}")
        lines += body
    return "\n".join(lines) + "\n"


def ran(points, pre=(), code=0, status=OK_STATUS):
    return d.run_outcome((code, output(points, pre), ""), status)


def same(body):
    return {k: list(body) for k in d.POINTS}


BODY = ["WANAMES a,b", "WAVALS 1 2", "OK -1.5 0.25 3"]


def kinds(diffs):
    return [k for k, _ in diffs]


class CloseTest(unittest.TestCase):
    def test_relative_and_absolute_floor(self):
        self.assertTrue(d.close("1", "1.00000000001"))
        self.assertFalse(d.close("1", "1.000000001"))
        self.assertTrue(d.close("0", "1e-13"))
        self.assertFalse(d.close("0", "1e-11"))

    def test_nonfinite(self):
        self.assertTrue(d.close("nan", "nan"))
        self.assertTrue(d.close("-inf", "-inf"))
        self.assertFalse(d.close("inf", "-inf"))
        self.assertFalse(d.close("nan", "1"))
        self.assertFalse(d.close("inf", "1e308"))

    def test_width(self):
        self.assertEqual(d.first_value_diff(["1", "2"], ["1"]),
                         ("width", 2, 1))
        self.assertEqual(d.first_value_diff(["1", "2"], ["1", "3"]),
                         (1, "2", "3"))


class CompareRunsTest(unittest.TestCase):
    def test_identical_runs_agree(self):
        self.assertEqual(d.compare_runs(ran(same(BODY)), ran(same(BODY))),
                         ([], None))

    def test_lp_grad_and_write_array(self):
        lp = same(BODY)
        lp[1] = ["WANAMES a,b", "WAVALS 1 2", "OK -1.6 0.25 3"]
        self.assertEqual(kinds(d.compare_runs(ran(same(BODY)), ran(lp))[0]),
                         ["lp"])
        grad = same(BODY)
        grad[2] = ["WANAMES a,b", "WAVALS 1 2", "OK -1.5 0.25"]
        diffs, _ = d.compare_runs(ran(same(BODY)), ran(grad))
        self.assertEqual(kinds(diffs), ["grad"])
        self.assertEqual(diffs[0][1]["index"], "width")
        wa = same(BODY)
        wa[0] = ["WANAMES a,b", "WAVALS 1 2.5", "OK -1.5 0.25 3"]
        diffs, _ = d.compare_runs(ran(same(BODY)), ran(wa))
        self.assertEqual(kinds(diffs), ["write_array"])
        self.assertEqual(diffs[0][1]["column"], "b")
        names = same(BODY)
        names[0] = ["WANAMES a,c", "WAVALS 1 2", "OK -1.5 0.25 3"]
        self.assertEqual(
            kinds(d.compare_runs(ran(same(BODY)), ran(names))[0]),
            ["write_array"])

    def test_tilde_programs_may_shift_lp_by_a_constant(self):
        # `~` drops terms that are constant for the types it sees, and O1 can
        # make an argument data, so the two sides may differ by a constant
        # with the same gradient.
        def body(lp, grad="0.25"):
            return ["WANAMES a,b", "WAVALS 1 2", f"OK {lp} {grad} 3"]
        none = {0: body(-1.5), 1: body(-1.4), 2: body(-1.3)}
        shifted = {0: body(-2.5), 1: body(-2.4), 2: body(-2.3)}
        self.assertEqual(kinds(d.compare_runs(ran(none), ran(shifted))[0]),
                         ["lp", "lp", "lp"])
        self.assertEqual(
            d.compare_runs(ran(none), ran(shifted), tilde=True), ([], None))
        uneven = {**shifted, 1: body(-2.5)}
        self.assertEqual(
            kinds(d.compare_runs(ran(none), ran(uneven), tilde=True)[0]),
            ["lp", "lp", "lp"])
        steeper = {**shifted, 2: body(-2.3, "0.5")}
        self.assertEqual(
            kinds(d.compare_runs(ran(none), ran(steeper), tilde=True)[0]),
            ["lp", "lp", "lp", "grad"])

    def test_throws_and_messages(self):
        threw = same(BODY)
        threw[0] = ["EVAL_FAIL bad scale 1.5"]
        self.assertIn("one_side_threw",
                      kinds(d.compare_runs(ran(same(BODY)), ran(threw))[0]))
        other = same(BODY)
        other[0] = ["EVAL_FAIL bad scale[2] 1.5"]
        diffs, _ = d.compare_runs(ran(threw), ran(other))
        self.assertEqual(kinds(diffs), ["error_message"])
        self.assertTrue(diffs[0][1]["normalized_agree"])
        nan = same(BODY)
        nan[0] = ["OK nan nan nan"]
        self.assertEqual(
            [k for k in kinds(d.compare_runs(ran(threw), ran(nan))[0])
             if k != "one_side_threw"], ["error_message"])

    def test_no_row_when_log_density_threw(self):
        # stanli_check runs write_array only after log_prob succeeds. An
        # interpreted section reports no columns before its first row, which
        # must not read as an empty row at a point where log_prob threw.
        threw = same(BODY)
        threw[0] = ["EVAL_FAIL bad scale 1.5"]
        interpreted = check_status.CheckStatus(
            {"record": "compile", "ok": True, "interpreter_fallbacks": ["gq"],
             "write_array": {"section": "interpreted", "columns": 0,
                             "truncated": "gq"}})
        self.assertEqual(
            d.compare_runs(ran(threw), ran(threw, status=interpreted)),
            ([], None))

    def test_prints(self):
        printed = {k: ["mu=1"] + BODY for k in d.POINTS}
        other = {k: ["mu=1"] + BODY for k in d.POINTS}
        other[2] = ["mu=2"] + BODY
        self.assertEqual(kinds(d.compare_runs(ran(printed), ran(other))[0]),
                         ["output"])
        self.assertEqual(d.compare_runs(ran(printed), ran(printed)),
                         ([], None))

    def test_engine_failures(self):
        refused = check_status.CheckStatus(
            {"record": "compile", "ok": False, "stage": "model",
             "error": "compile_error", "message": "m"})
        rejected = check_status.CheckStatus(
            {"record": "compile", "ok": False, "stage": "data",
             "error": "domain_error", "message": "m"})
        good = ran(same(BODY))
        no = d.run_outcome((1, "COMPILE_FAIL unsupported thing\n", ""),
                           refused)
        self.assertEqual(d.compare_runs(no, good), ([], "stanli_refuses_none"))
        self.assertEqual(d.compare_runs(no, no), ([], "both_refused"))
        self.assertEqual(kinds(d.compare_runs(good, no)[0]),
                         ["compile_fail_o1"])
        data = d.run_outcome((1, "COMPILE_FAIL data: N < 0\n", ""), rejected)
        self.assertEqual(d.compare_runs(data, data), ([], None))
        self.assertEqual(kinds(d.compare_runs(good, data)[0]),
                         ["one_side_threw"])
        crash = d.run_outcome((-11, "", "Segmentation fault"), OK_STATUS)
        self.assertEqual(crash[0], "crash")
        self.assertEqual(kinds(d.compare_runs(good, crash)[0]), ["crash"])
        self.assertEqual(kinds(d.compare_runs(d.run_outcome(None, OK_STATUS),
                                              good)[0]), ["timeout"])


class CompileAndBucketTest(unittest.TestCase):
    def compiled(self, outcome):
        return {"outcome": outcome, "message": "m"}

    def test_compile_outcomes(self):
        ok, bad = self.compiled("ok"), self.compiled("internal")
        self.assertEqual(d.compare_compiles(ok, ok), (None, None))
        self.assertEqual(d.compare_compiles(bad, bad),
                         ([], "both_compile_fail"))
        self.assertEqual(kinds(d.compare_compiles(ok, bad)[0]),
                         ["compile_fail_o1"])
        self.assertEqual(kinds(d.compare_compiles(bad, ok)[0]),
                         ["compile_fail_o0"])
        self.assertEqual(
            d.compare_compiles(self.compiled("unsupported"), ok),
            ([], "compiler_unsupported_none"))
        self.assertEqual(
            kinds(d.compare_compiles(ok, self.compiled("timeout"))[0]),
            ["timeout"])

    def test_compiler_failures_skip_warnings(self):
        warning = ("warning: Assignment of variable to itself.\n"
                   "    \u250c\u2500 m.stan:3:5\n  3 \u2502  x = x;\n\n")
        self.assertEqual(
            d.classify_failure(1, warning + 'Error: Identifier "y" not in '
                               "scope.\n    at m.stan:2\n"),
            ("frontend", 'Error: Identifier "y" not in scope.\n'
                         "    at m.stan:2"))
        self.assertEqual(d.classify_failure(1, "error: Ill-formed\n\nmore"),
                         ("frontend", "error: Ill-formed\n\nmore"))
        self.assertEqual(
            d.classify_failure(1, warning + "stanli does not support x yet"),
            ("unsupported", "stanli does not support x yet"))
        self.assertEqual(
            d.classify_failure(1, "Internal compiler error: boom"),
            ("internal", "Internal compiler error: boom"))
        self.assertEqual(d.classify_failure(-6, "abort"), ("crash", "abort"))

    def test_bucket_uses_first_kind_in_priority_order(self):
        diffs = [("output", {}), ("grad", {}), ("lp", {})]
        self.assertEqual(d.bucket("copy_propagation", diffs),
                         "copy_propagation:lp")
        self.assertEqual(d.primary_kind([("error_message", {}),
                                         ("one_side_threw", {})]),
                         "one_side_threw")

    def test_bisect_steps_always_collapse_lists(self):
        steps = d.bisect_steps()
        self.assertEqual(steps[0], ("list_collapsing", "list_collapsing"))
        self.assertEqual(steps[1], ("constant_propagation",
                                    "constant_propagation,list_collapsing"))
        for added, spec in steps:
            self.assertIn("list_collapsing", spec.split(","))
            self.assertIn(added, spec.split(","))
        self.assertEqual(steps[-1][1], ",".join(d.PRODUCTION_PASSES))
        self.assertEqual([a for a, _ in steps[1:]],
                         [p for p in d.PRODUCTION_PASSES
                          if p != "list_collapsing"])

    def test_guilty_step_must_show_the_o1_kind(self):
        steps = [{"added": "list_collapsing", "primary": None},
                 {"added": "function_inlining",
                  "primary": "compile_fail_o1"},
                 {"added": "constant_propagation", "primary": None},
                 {"added": "dead_code_elimination",
                  "primary": "write_array"}]
        self.assertEqual(d.guilty_step(steps, "write_array"),
                         "dead_code_elimination")
        self.assertEqual(d.guilty_step(steps, "compile_fail_o1"),
                         "function_inlining")
        self.assertEqual(d.guilty_step(steps, "lp"), "function_inlining?")
        self.assertEqual(d.guilty_step(steps[:1], "lp"), "unreproduced")

    def test_guilty_refusal_must_have_the_o1_message(self):
        steps = [{"added": "function_inlining", "primary": "compile_fail_o1",
                  "detail": "region produces nothing"},
                 {"added": "copy_propagation", "primary": "compile_fail_o1",
                  "detail": "logical view mismatch for V"}]
        self.assertEqual(d.guilty_step(steps, "compile_fail_o1",
                                       "logical view mismatch for V"),
                         "copy_propagation")
        self.assertEqual(d.detail([("compile_fail_o1", {
            "stanli_refused": "logical view mismatch for inline_f4_a6_arg10__"
        })]), "logical view mismatch for V")
        self.assertIsNone(d.detail([("lp", {"none": "1", "o1": "2"})]))

    def test_production_passes_follow_the_suite(self):
        self.assertEqual(d.PRODUCTION_PASSES[0], "constant_propagation")
        self.assertEqual(d.PRODUCTION_PASSES[-1], "list_collapsing")
        self.assertEqual(len(set(d.PRODUCTION_PASSES)), 5)


if __name__ == "__main__":
    unittest.main()
