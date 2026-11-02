#!/usr/bin/env python3
"""Tests for tools/check_stanc3_good.py's verdicts and ledger gate.

Synthetic driver output stands in for stanli_check and CmdStan, so nothing
here needs a build, CmdStan or the stanc3 checkout.
"""
from __future__ import annotations

import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import check_stanc3_good as g  # noqa: E402


def stanli(points, pre=None, code=0):
    """stanli_check --points output: {k: (status line, wa lines)}."""
    lines = [pre] if pre else []
    for k, (status, wa) in sorted(points.items()):
        lines.append(f"POINT {k}")
        lines += wa
        lines.append(status)
    return code, "\n".join(lines) + "\n", ""


WA = ["WANAMES a,b", "WAVALS 1 2"]


def ref_ok(lp_grad=("-1", "0.5"), wa=(["a", "b"], ["1", "2"])):
    return {"outcome": "ok", "points": {
        str(k): {"lp_grad": list(lp_grad),
                 "wa": {"names": wa[0], "values": wa[1]}} for k in g.POINTS}}


def all_points(status, wa=WA):
    return {k: (status, wa) for k in g.POINTS}


class ImportTest(unittest.TestCase):
    def test_replay_does_not_import_cmdstan_ref(self):
        self.assertNotIn("cmdstan_ref", sys.modules)


class ParseTest(unittest.TestCase):
    def test_last_status_wins_and_points_split(self):
        out = "POINT 0\nOK 1 2\nWANAMES x\nWAVALS 3\nPOINT 1\n" \
              "OK print text\nEVAL_FAIL bad: y is 1, but must be positive\n"
        parsed = g.parse_output(out)
        self.assertEqual(parsed["points"][0]["status"], ("OK", ["1", "2"]))
        self.assertEqual(parsed["points"][0]["wa"], ("ok", ["x"], ["3"]))
        self.assertEqual(parsed["points"][1]["status"][0], "EVAL_FAIL")

    def test_empty_write_array_row(self):
        parsed = g.parse_output("POINT 0\nWANAMES \nWAVALS\nOK 0\n")
        self.assertEqual(parsed["points"][0]["wa"], ("ok", [], []))
        parsed = g.parse_output("POINT 0\nWANAMES\nWAVALS\nOK 0\n")
        self.assertEqual(parsed["points"][0]["wa"], ("ok", [], []))

    def test_compile_fail_before_points(self):
        parsed = g.parse_output("COMPILE_FAIL unsupported function foo\n")
        self.assertEqual(parsed["pre"], ("COMPILE_FAIL",
                                         "unsupported function foo"))


class ClassifyTest(unittest.TestCase):
    def test_verified(self):
        self.assertEqual(g.classify(ref_ok(), stanli(all_points("OK -1 0.5")))[0],
                         "verified")

    def test_gradient_mismatch_is_named_per_point(self):
        points = all_points("OK -1 0.5")
        points[3] = ("OK -1 0.6", WA)
        status, bad, details = g.classify(ref_ok(), stanli(points))
        self.assertEqual((status, bad), ("points", {"3": "mismatch"}))
        self.assertIn("lp_grad[1]", details[0])

    def test_infinities_of_opposite_sign_mismatch(self):
        points = all_points("OK -1 0.5")
        points[0] = ("OK inf 0.5", WA)
        ref = ref_ok()
        ref["points"]["0"]["lp_grad"] = ["-inf", "0.5"]
        self.assertEqual(g.classify(ref, stanli(points))[1], {"0": "mismatch"})

    def test_same_nan_agrees(self):
        ref = ref_ok(lp_grad=("-1", "nan"))
        self.assertEqual(g.classify(ref, stanli(all_points("OK -1 nan")))[0],
                         "verified")

    def test_write_array_value_and_names(self):
        points = all_points("OK -1 0.5")
        points[1] = ("OK -1 0.5", ["WANAMES a,b", "WAVALS 1 2.5"])
        points[2] = ("OK -1 0.5", ["WANAMES b,a", "WAVALS 1 2"])
        status, bad, details = g.classify(ref_ok(), stanli(points))
        self.assertEqual(bad, {"1": "mismatch", "2": "mismatch"})
        self.assertTrue(any("write_array[b]" in d for d in details))

    def test_both_threw_needs_the_message_core(self):
        ref = ref_ok()
        ref["points"]["2"] = {"threw": "Exception: normal_lpdf: Scale "
                              "parameter is -0.1, but must be positive! "
                              "(in 'm.stan', line 4, column 2 to column 20)"}
        points = all_points("OK -1 0.5")
        points[2] = ("EVAL_FAIL normal_lpdf: Scale parameter is "
                     "-0.10000000000000001, but must be positive!", [])
        self.assertEqual(g.classify(ref, stanli(points))[0], "verified")
        points[2] = ("EVAL_FAIL something else entirely", [])
        self.assertEqual(g.classify(ref, stanli(points))[1],
                         {"2": "message_mismatch"})

    def test_boost_template_argument_is_platform_dependent(self):
        cmdstan = ("Exception: Error in function boost::math::gamma_p<long "
                   "double>(long double, long double): Argument a to the "
                   "incomplete gamma function must be greater than zero "
                   "(got a=0). (in 'm.stan', line 4, column 2 to column 20)")
        stanli_text = cmdstan.split("Exception: ")[1].split(" (in")[0]
        for argument in ("double", "long double", "__float128"):
            text = stanli_text.replace("<long double>(long double, long "
                                       "double)", f"<{argument}>({argument}, "
                                       f"{argument})")
            self.assertTrue(g.messages_agree(cmdstan, text), argument)
        self.assertFalse(g.messages_agree(
            cmdstan, stanli_text.replace("gamma_p", "gamma_q")))
        self.assertFalse(g.messages_agree(
            "Exception: foo<double> is bad", "foo<long double> is bad"))

    def test_all_nan_rows_agree(self):
        ref = ref_ok(lp_grad=("nan", "nan"))
        self.assertEqual(g.classify(ref, stanli(all_points("OK nan nan")))[0],
                         "rejected_both")
        self.assertEqual(g.classify(ref, stanli(all_points("OK -1 0.5")))[1],
                         {str(k): "one_side_threw" for k in g.POINTS})

    def test_one_side_threw_either_way(self):
        ref = ref_ok()
        ref["points"]["1"] = {"threw": "boom"}
        points = all_points("OK -1 0.5")
        points[0] = ("EVAL_FAIL nope", [])
        self.assertEqual(g.classify(ref, stanli(points))[1],
                         {"0": "one_side_threw", "1": "one_side_threw"})

    def test_all_points_refused_by_both(self):
        ref = {"outcome": "ok",
               "points": {str(k): {"threw": "f: x is 1, but must be 2"}
                          for k in g.POINTS}}
        run = stanli(all_points("EVAL_FAIL f: x is 1.0, but must be 2", []))
        self.assertEqual(g.classify(ref, run)[0], "rejected_both")

    def test_compile_failures(self):
        ref = ref_ok()
        run = stanli({}, pre="COMPILE_FAIL unsupported function foo", code=1)
        self.assertEqual(g.classify(ref, run)[0], "unsupported")
        run = stanli({}, pre="COMPILE_FAIL f: y is 3, but must be <= 2", code=1)
        self.assertEqual(g.classify(ref, run)[0], "one_side_threw")
        rejected = {"outcome": "data_fail",
                    "message": "Exception: f: y is 3, but must be <= 2 (in 'x')"}
        self.assertEqual(g.classify(rejected, run)[0], "data_rejected")
        self.assertEqual(g.classify(rejected, stanli(all_points("OK -1 0.5")))[0],
                         "one_side_threw")

    def test_timeouts_and_crashes(self):
        self.assertEqual(g.classify(ref_ok(), None)[0], "timeout")
        self.assertEqual(g.classify({"outcome": "timeout"}, None)[0],
                         "cmdstan_timeout")
        self.assertEqual(g.classify(ref_ok(), (-11, "", ""))[0], "crashed")


class LedgerTest(unittest.TestCase):
    def test_platform_dependent_point_tolerates_only_value_mismatches(self):
        points = all_points("OK -1 0.5")
        points[4] = ("OK -1 nan", WA)
        self.assertEqual(g.classify(ref_ok(), stanli(points), {"4"})[0],
                         "verified")
        self.assertEqual(g.classify(ref_ok(), stanli(points), {"3"})[:2],
                         ("points", {"4": "mismatch"}))
        points[4] = ("EVAL_FAIL boom", WA)
        self.assertEqual(g.classify(ref_ok(), stanli(points), {"4"})[:2],
                         ("points", {"4": "one_side_threw"}))
        entry = {"platform_dependent": ["4"], "cause": "libm"}
        self.assertIsNone(g.ledger_verdict("verified", {}, entry))
        self.assertEqual(g.inventory_problems([], {"models": {}}, {}, {}), [])
        self.assertEqual(g.update_ledger({"a.stan": entry},
                                         {"a.stan": ("verified", {}, [])}),
                         {"a.stan": entry})

    def test_verified_needs_no_entry(self):
        self.assertIsNone(g.ledger_verdict("verified", {}, None))

    def test_regression_without_entry_fails(self):
        problem = g.ledger_verdict("unsupported", {}, None)
        self.assertIn("REGRESSED", problem)

    def test_stale_entry_fails(self):
        problem = g.ledger_verdict("verified", {}, {"status": "unsupported",
                                                     "cause": "foo"})
        self.assertIn("improved", problem)

    def test_point_set_must_match(self):
        entry = {"points": {"1": "mismatch"}, "cause": "singular"}
        self.assertIsNone(g.ledger_verdict("points", {"1": "mismatch"}, entry))
        self.assertIsNotNone(g.ledger_verdict(
            "points", {"1": "mismatch", "2": "mismatch"}, entry))
        self.assertIn("improved", g.ledger_verdict("verified", {}, entry))

    def test_untriaged_cause_fails(self):
        entry = {"status": "timeout", "cause": "UNTRIAGED: stanli hangs"}
        self.assertIn("named cause", g.ledger_verdict("timeout", {}, entry))

    def test_update_keeps_causes_that_still_apply(self):
        old = {"a.stan": {"status": "unsupported", "cause": "kept"},
               "b.stan": {"status": "unsupported", "cause": "dropped"}}
        results = {"a.stan": ("unsupported", {}, ["new message"]),
                   "b.stan": ("verified", {}, []),
                   "c.stan": ("points", {"0": "mismatch"}, ["lp_grad[0]"])}
        new = g.update_ledger(old, results)
        self.assertEqual(new["a.stan"]["cause"], "kept")
        self.assertNotIn("b.stan", new)
        self.assertTrue(new["c.stan"]["cause"].startswith("UNTRIAGED"))


if __name__ == "__main__":
    unittest.main()
