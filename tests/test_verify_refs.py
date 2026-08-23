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

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from verify_refs import POINTS, check_model, probe_point  # noqa: E402

# A model that exists under tests/stanc3, so model_files resolves real
# paths without unpacking a posteriordb dataset. The stub never reads
# them; they only have to be there.
MODEL = "reductions_allowed"


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


class ProbePointTest(unittest.TestCase):
    """One run, one verdict. probe_point returns ("OK", "") or a failure."""

    def verdict(self, body, point=1):
        with tempfile.TemporaryDirectory() as tmp:
            return probe_point(MODEL, REPO / "tests" / "stanc3" / f"{MODEL}.stan",
                               REPO / "tests" / "stanc3" / f"{MODEL}.json",
                               stub(tmp, body), point, 60)

    def test_finite_lp_and_gradients_pass(self):
        self.assertEqual(self.verdict('echo "OK -3.5 1 -2"')[0], "OK")

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
    """The replay evaluates every point, not the recorded one alone."""

    REF = {"point": 0, "values": ["-3.5", "1", "-2"]}

    def run_check(self, body):
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(MODEL, self.REF, REPO / "nonexistent-pdb",
                               stub(tmp, body), pathlib.Path(tmp), 60)

    def test_agreeing_at_every_point_passes(self):
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"')[1], "OK")

    def test_crash_away_from_the_recorded_point_fails(self):
        # reductions_allowed in miniature: the recorded point matches the
        # reference exactly, and the run still has to fail.
        _, status, _, _, _, detail = self.run_check(
            'if [ "$point" = 2 ]; then kill -SEGV $$; fi\necho "OK -3.5 1 -2"')
        self.assertEqual(status, "CRASH")
        self.assertIn("point 2", detail)
        self.assertIn("signal 11", detail)

    def test_every_point_is_visited(self):
        # Each point appends its number to a witness file; all three must
        # be there when the model passes.
        with tempfile.TemporaryDirectory() as tmp:
            seen = pathlib.Path(tmp) / "seen"
            result = check_model(
                MODEL, self.REF, REPO / "nonexistent-pdb",
                stub(tmp, f'echo "$point" >> {seen}\necho "OK -3.5 1 -2"'),
                pathlib.Path(tmp), 60)
            self.assertEqual(result[1], "OK")
            self.assertEqual(sorted(seen.read_text().split()),
                             [str(p) for p in POINTS])


if __name__ == "__main__":
    unittest.main()
