#!/usr/bin/env python3
"""Tests for the corpus replay's per-point verdicts.

tools/verify_refs.py is the strongest oracle in the project and had no
test of its own. What is tested here is the part that decides whether a
run at a point with no recorded reference passed -- in particular that a
crash is reported as a crash, which is the failure a month of green
corpus runs missed while reductions_allowed segfaulted at point 2.

A stub stanli_check (a shell script that prints whatever the case wants,
or kills itself) stands in for the real binary, so the verdicts are
exercised without a build.
"""

from __future__ import annotations

import pathlib
import stat
import sys
import tempfile
import unittest
import unittest.mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from verify_refs import (POINTS, QUARANTINED, SCHEMA,  # noqa: E402
                         check_model, load_refs, parse_status, probe_point)
from cmdstan_ref import _result_line  # noqa: E402
from verify_sample import evaluate, record_wa  # noqa: E402

# A model that exists under tests/stanc3, so model_files resolves real
# paths without unpacking a posteriordb dataset. The stub never reads
# them; they only have to be there.
MODEL = "reductions_allowed"
# One point's reference: lp and two gradients, agreeing with CmdStan.
VALUES = {"values": ["-3.5", "1", "-2"], "status": "VERIFIED",
          "max_rel": 0.0, "max_ulp": 0}
REF = {"primary": 0, "points": {str(p): VALUES for p in POINTS}}


def stub(tmp, body):
    """A stanli_check that runs `body` with $point set to --point's value."""
    path = pathlib.Path(tmp) / "stanli_check_stub.sh"
    path.write_text("#!/bin/sh\npoint=0\n"
                    'while [ $# -gt 0 ]; do\n'
                    '  [ "$1" = --point ] && point=$2\n'
                    '  shift\n'
                    "done\n" + body + "\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class StatusLineTest(unittest.TestCase):
    """Machine results may follow output from transformed-data prints."""

    def test_each_result_kind_is_found_after_printed_output(self):
        for line in ("OK -3.5 1 -2", "COMPILE_FAIL no MIR",
                     "EVAL_FAIL out of range"):
            with self.subTest(line=line):
                self.assertEqual(parse_status(f"mother = 1\n{line}\n"),
                                 line.split())

    def test_output_without_a_result_has_no_status(self):
        self.assertEqual(parse_status("mother = 1\n"), [])

    def test_a_printed_status_prefix_does_not_shadow_the_driver_result(self):
        self.assertEqual(
            parse_status("OK user text\nEVAL_FAIL out of range\n"),
            ["EVAL_FAIL", "out", "of", "range"])

    def test_generated_quantities_print_precedes_final_numeric_result(self):
        out = "OK generated quantity text\nOK -3.5 1 -2\n"
        want = ["OK", "-3.5", "1", "-2"]
        self.assertEqual(parse_status(out), want)
        self.assertEqual(_result_line(out), want)


class ProbePointTest(unittest.TestCase):
    """One run, one verdict. probe_point returns ("OK", "") or a failure."""

    def verdict(self, body, point=1):
        with tempfile.TemporaryDirectory() as tmp:
            return probe_point(MODEL, REPO / "tests" / "stanc3" / f"{MODEL}.stan",
                               REPO / "tests" / "stanc3" / f"{MODEL}.json",
                               stub(tmp, body), point, 60)

    def test_finite_lp_and_gradients_pass(self):
        self.assertEqual(self.verdict('echo "OK -3.5 1 -2"')[0], "OK")

    def test_printed_output_before_result_is_ignored(self):
        self.assertEqual(self.verdict(
            'echo "mother = 1"\necho "OK -3.5 1 -2"')[0], "OK")

    def test_signal_is_a_crash(self):
        status, detail = self.verdict("kill -SEGV $$")
        self.assertEqual(status, "CRASH")
        self.assertIn("signal 11", detail)

    def test_silent_nonzero_exit_is_a_crash(self):
        status, detail = self.verdict("exit 3")
        self.assertEqual(status, "CRASH")
        self.assertIn("exit 3, no output", detail)

    def test_clean_rejection_passes(self):
        # The model is outside its declared support at this point and says
        # so; CmdStan throws in the same place. Not a failure.
        self.assertEqual(self.verdict('echo "EVAL_FAIL out of range"')[0], "OK")

    def test_zero_density_passes(self):
        # log(0) is a value both engines produce and agree on.
        self.assertEqual(self.verdict('echo "OK -inf 0 0"')[0], "OK")

    def test_nan_lp_fails(self):
        self.assertEqual(self.verdict('echo "OK nan 0 0"')[0],
                         "POINT_NONFINITE_LP")

    def test_nonfinite_gradient_under_finite_lp_fails(self):
        status, detail = self.verdict('echo "OK -3.5 1 nan"')
        self.assertEqual(status, "POINT_NONFINITE_GRAD")
        self.assertIn("1/2", detail)

    def test_compile_fail_is_not_a_rejection(self):
        # Compiling does not depend on the point, so a model that
        # compiled at its recorded point must compile at every other one.
        self.assertEqual(self.verdict('echo "COMPILE_FAIL no MIR"')[0],
                         "POINT_COMPILE_FAIL")


class CheckModelPointsTest(unittest.TestCase):
    """The replay holds every point to its own recorded reference."""

    REF = REF

    def run_check(self, body, ref=None, model=MODEL):
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(model, ref or self.REF,
                               REPO / "nonexistent-pdb", stub(tmp, body),
                               pathlib.Path(tmp), 60, 1e-9)

    def test_agreeing_at_every_point_passes(self):
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"')[1], "OK")

    def test_printed_output_before_each_result_is_ignored(self):
        self.assertEqual(self.run_check(
            'echo "mother = $point"\necho "OK -3.5 1 -2"')[1], "OK")

    def test_crash_away_from_the_recorded_point_fails(self):
        # reductions_allowed in miniature: the recorded point matches the
        # reference exactly, and the run still has to fail.
        _, status, _, _, _, detail, _ = self.run_check(
            'if [ "$point" = 2 ]; then kill -SEGV $$; fi\necho "OK -3.5 1 -2"')
        self.assertEqual(status, "CRASH")
        self.assertIn("point 2", detail)
        self.assertIn("signal 11", detail)

    def test_wrong_value_away_from_the_primary_point_fails(self):
        # The whole reason every point carries a reference: a finite
        # gradient that is simply wrong, where nothing used to compare it.
        _, status, rel, _, _, detail, _ = self.run_check(
            'if [ "$point" = 1 ]; then echo "OK -3.5 1 -2.5"; '
            'else echo "OK -3.5 1 -2"; fi')
        self.assertEqual(status, "GATE")
        self.assertIn("point 1", detail)
        self.assertGreater(rel, 1e-9)

    def test_every_point_is_visited(self):
        # Each point appends its number to a witness file; all three must
        # be there when the model passes.
        with tempfile.TemporaryDirectory() as tmp:
            seen = pathlib.Path(tmp) / "seen"
            result = check_model(
                MODEL, self.REF, REPO / "nonexistent-pdb",
                stub(tmp, f'echo "$point" >> {seen}\necho "OK -3.5 1 -2"'),
                pathlib.Path(tmp), 60, 1e-9)
            self.assertEqual(result[1], "OK")
            self.assertEqual(sorted(seen.read_text().split()),
                             [str(p) for p in POINTS])

    def test_a_recorded_rejection_must_be_reproduced(self):
        # CmdStan refuses this point, so the reference holds no values.
        # stanli refusing it too is agreement.
        ref = {"primary": 0, "points": dict(REF["points"],
                                            **{"1": {"status":
                                                     "REJECTED_BOTH"}})}
        self.assertEqual(self.run_check(
            'if [ "$point" = 1 ]; then echo "EVAL_FAIL out of range"; '
            'else echo "OK -3.5 1 -2"; fi', ref)[1], "OK")

    def test_accepting_a_point_cmdstan_rejects_fails(self):
        ref = {"primary": 0, "points": dict(REF["points"],
                                            **{"1": {"status":
                                                     "REJECTED_BOTH"}})}
        _, status, _, _, _, detail, _ = self.run_check(
            'echo "OK -3.5 1 -2"', ref)
        self.assertEqual(status, "POINT_NOT_REJECTED")
        self.assertIn("point 1", detail)

    def test_a_quarantined_point_is_announced_and_not_compared(self):
        # The reference is recorded and deliberately not enforced. The
        # point still has to run without crashing, and every run has to
        # say out loud that it was skipped. Quarantining this test's own
        # model rather than reading whoever is in QUARANTINED today keeps
        # the test alive after the last real entry is deleted.
        with unittest.mock.patch.dict(QUARANTINED, {(MODEL, 1): "test"}):
            _, status, _, _, _, _, notes = self.run_check(
                'if [ "$point" = 1 ]; then echo "OK -3.5 1 nan"; '
                'else echo "OK -3.5 1 -2"; fi')
        self.assertEqual(status, "OK")
        self.assertIn(f"QUARANTINE {MODEL} point 1: test", notes)


class RecorderEvaluateTest(unittest.TestCase):
    """The recorder extracts both engines' statuses from full stdout."""

    def test_printed_output_before_results_is_ignored(self):
        runs = [
            unittest.mock.Mock(stdout="reference print\nOK -3.5 1 -2\n"),
            unittest.mock.Mock(stdout="mother = 1\nOK -3.5 1 -2\n"),
        ]
        with unittest.mock.patch("verify_sample.subprocess.run",
                                 side_effect=runs):
            ref, ref_out, got, got_out = evaluate(
                pathlib.Path("ref"), pathlib.Path("stanli_check"),
                pathlib.Path("mother.stan"), pathlib.Path("mother.json"), 1)
        self.assertEqual(ref, ["OK", "-3.5", "1", "-2"])
        self.assertEqual(got, ref)
        self.assertTrue(ref_out.startswith("reference print"))
        self.assertTrue(got_out.startswith("mother = 1"))

    def test_rng_write_array_values_are_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            stan = pathlib.Path(tmp) / "rng.stan"
            stan.write_text("generated quantities { real x = normal_rng(0, 1); }")
            point = {}
            note = record_wa(
                stan, point,
                "WANAMES x\nWAVALS 0.125\n",
                "WANAMES x\nWAVALS 0.125\n")
        self.assertEqual(point["wa"], {"names": "x", "values": ["0.125"]})
        self.assertIn("1 values recorded", note)


class SchemaTest(unittest.TestCase):
    """The committed reference file, and what happens to an older one."""

    def test_the_committed_file_loads_and_covers_every_point(self):
        models, recorded = load_refs()
        self.assertTrue(models)
        for name in ("cmdstan", "stan", "math", "stanc3", "posteriordb"):
            self.assertIn(name, recorded)
        for model, ref in models.items():
            self.assertEqual(sorted(ref["points"]),
                             sorted(str(p) for p in POINTS), model)

    def test_an_older_schema_is_refused_not_half_read(self):
        # Reading schema 1's one-point entries as if they were this format
        # would raise KeyError deep inside a worker thread, or worse,
        # compare nothing and report a pass.
        import gzip
        import json
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "old.json.gz"
            path.write_bytes(gzip.compress(json.dumps(
                {"m": {"point": 0, "values": ["1"]}}).encode()))
            with self.assertRaises(SystemExit) as caught:
                load_refs(path)
            self.assertIn(f"schema {SCHEMA}", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
