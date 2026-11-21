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
import copy
import json
import math
import stat
import sys
import tempfile
import unittest
import unittest.mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from verify_refs import (ILL_CONDITIONED, KNOWN_GAPS,  # noqa: E402
                         POINTS, QUARANTINED, SCHEMA,
                         check_model, load_refs, model_files, parse_status,
                         probe_point)
from cmdstan_ref import point_results, split_points  # noqa: E402
import verify_sample  # noqa: E402
import toolchain  # noqa: E402
import verify_refs  # noqa: E402
import check_status  # noqa: E402
from corpus_inventory import local_cases, source_digest
from verify_sample import evaluate, record_wa, write_refs  # noqa: E402

# A model that exists under tests/regressions, so model_files resolves real
# paths without unpacking a posteriordb dataset. The stub never reads
# them; they only have to be there.
MODEL = "while_counter_size"
# One point's reference: lp and two gradients, agreeing with CmdStan.
VALUES = {"values": ["-3.5", "1", "-2"], "status": "VERIFIED",
          "max_rel": 0.0, "max_ulp": 0}
REF = {"primary": 0, "points": {str(p): VALUES for p in POINTS}}


def compiled(fallbacks=(), section="graph", truncated=None):
    """Shell that writes stanli_check's compile record to $status."""
    record = {"record": "compile", "ok": True,
              "interpreter_fallbacks": list(fallbacks),
              "write_array": {"section": section, "columns": 1,
                              "truncated": truncated}}
    return f"echo '{json.dumps(record)}' > \"$status\"\n"


def stub(tmp, body, wa=True):
    """A stanli_check that runs `body` with $point set to --point's value.

    A real one answers --wa-values with a write_array row and the replay
    demands one, so the stub emits a row too. Cases about the row itself
    pass wa=False and write their own. It writes a compile record with no
    interpreted parts to the --status-json file before `body` runs.
    """
    path = pathlib.Path(tmp) / "stanli_check_stub.sh"
    row = ('if [ -n "$wa" ]; then echo "WANAMES a"; echo "WAVALS 1"; fi\n'
           if wa else "")
    path.write_text("#!/bin/sh\npoint=0\nwa=\nstatus=/dev/null\n"
                    'while [ $# -gt 0 ]; do\n'
                    '  [ "$1" = --point ] && point=$2\n'
                    '  [ "$1" = --wa-values ] && wa=1\n'
                    '  [ "$1" = --status-json ] && status=$2\n'
                    '  shift\n'
                    "done\n" + compiled() + row + body + "\n")
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


class MultiPointOutputTest(unittest.TestCase):
    """`--points` output is one POINT <k> block per point, on both sides."""

    OUT = ("POINT 0\nOK -1.5 0.25 2\nWANAMES a,b\nWAVALS 1 2\n"
           "POINT 3\nEVAL_FAIL bad | worse\n"
           "POINT 1\nuser print\nOK -2 3 4\n")

    def test_blocks_keep_order_and_their_own_lines(self):
        blocks = split_points(self.OUT)
        self.assertEqual(list(blocks), [0, 3, 1])
        self.assertEqual(blocks[0], "OK -1.5 0.25 2\nWANAMES a,b\nWAVALS 1 2\n")
        self.assertEqual(blocks[3], "EVAL_FAIL bad | worse\n")

    def test_each_block_parses_like_a_single_point_run(self):
        self.assertEqual(point_results(self.OUT), {
            0: ["OK", "-1.5", "0.25", "2"],
            3: ["EVAL_FAIL", "bad", "|", "worse"],
            1: ["OK", "-2", "3", "4"]})

    def test_a_throw_at_one_point_does_not_hide_the_others(self):
        self.assertEqual(point_results(self.OUT)[1][0], "OK")

    def test_a_failure_before_any_point_is_returned_whole(self):
        for line in ("COMPILE_FAIL no MIR", "DATA_FAIL bad data"):
            with self.subTest(line=line):
                self.assertEqual(split_points(line + "\n"), {})
                self.assertEqual(parse_status(line + "\n"), line.split())

    def test_a_printed_point_prefix_is_not_a_header(self):
        out = "POINT 0\nPOINT lies\nOK 1 2\n"
        self.assertEqual(point_results(out), {0: ["OK", "1", "2"]})


class ProbePointTest(unittest.TestCase):
    """One run, one verdict. probe_point returns ("OK", "") or a failure."""

    def verdict(self, body, point=1):
        with tempfile.TemporaryDirectory() as tmp:
            return probe_point(MODEL, REPO / "tests" / "regressions" / f"{MODEL}.stan",
                               REPO / "tests" / "regressions" / f"{MODEL}.json",
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

    def run_check(self, body, ref=None, model=MODEL, wa=True):
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(model, ref or self.REF,
                               REPO / "nonexistent-pdb", stub(tmp, body, wa),
                               pathlib.Path(tmp), 60, 1e-9)

    def test_agreeing_at_every_point_passes(self):
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"')[1], "OK")

    def test_interpreter_use_must_be_listed(self):
        interp = compiled(["generated quantities"]) + 'echo "OK -3.5 1 -2"'
        self.assertEqual(self.run_check(interp)[1], "INTERPRETER")
        with unittest.mock.patch.dict(verify_refs.CORPUS_INTERPRETED,
                                      {MODEL: "a test"}):
            self.assertEqual(self.run_check(interp)[1], "OK")
            self.assertEqual(self.run_check('echo "OK -3.5 1 -2"')[1],
                             "INTERPRETER")

    def test_tight_ulp_gate_catches_small_scaled_error(self):
        ref = {**REF, "recorded": {"platform": verify_refs.native_platform()}}
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            for distance, verdict in ((10, "OK"), (11, "ULP_GATE"),
                                      (256, "ULP_GATE")):
                value = 1.0
                for _ in range(distance):
                    value = math.nextafter(value, math.inf)
                result = self.run_check(f'echo "OK -3.5 {value!r} -2"', ref)
                self.assertEqual(result[1], verdict, distance)
                if verdict == "ULP_GATE":
                    self.assertLess(result[2], 1e-9)
                    self.assertEqual(result[3], distance)

    def test_cross_platform_rounding_keeps_the_scaled_error_gate(self):
        ref = {**REF, "recorded": {"platform": "another platform"}}
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            rounded = 1.0 + 256 * math.ulp(1.0)
            result = self.run_check(f'echo "OK -3.5 {rounded!r} -2"', ref)
            self.assertEqual(result[1], "OK")
            self.assertEqual(result[3], 256)
            self.assertEqual(self.run_check('echo "OK -3.5 1.000001 -2"', ref)[1],
                             "GATE")
            self.assertEqual(self.run_check('echo "OK -3.5 nan -2"', ref)[1],
                             "GATE")

    def test_gradient_against_largest_entry_replaces_per_element_ulp(self):
        wide = {"values": ["-3.5", "1000", "0"], "status": "VERIFIED",
                "max_rel": 0.0, "max_ulp": 0}
        ref = {"primary": 0, "points": {str(p): wide for p in POINTS},
               "recorded": {"platform": verify_refs.native_platform()}}
        step = math.ulp(1000.0)
        small = f'echo "OK -3.5 1000 {5 * step!r}"'
        large = f'echo "OK -3.5 1000 {11 * step!r}"'
        lp = f'echo "OK {-3.5 + 11 * math.ulp(3.5)!r} 1000 0"'
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            self.assertEqual(self.run_check(small, ref)[1], "ULP_GATE")
            with unittest.mock.patch.dict(verify_refs.GRADIENT_AGAINST_LARGEST,
                                          {MODEL: "a test"}):
                result = self.run_check(small, ref)
                self.assertEqual(result[1], "OK")
                self.assertEqual(result[3], 5)
                self.assertEqual(self.run_check(large, ref)[1], "ULP_GATE")
                self.assertEqual(self.run_check(lp, ref)[1], "ULP_GATE")

    def test_missing_ulp_reference_provenance_fails(self):
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            self.assertEqual(self.run_check('echo "OK -3.5 1 -2"')[1],
                             "REFERENCE_INCOMPLETE")

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

    def test_an_all_nan_row_is_the_other_spelling_of_a_rejection(self):
        # stanli_check prints its values rather than refusing them, so a
        # point it cannot evaluate comes back as OK with a row of nan.
        # The recorder reads that row as a rejection (verify_sample's
        # accepted) and records REJECTED_BOTH; the replay has to read it
        # the same way.
        ref = {"primary": 0, "points": dict(REF["points"],
                                            **{"1": {"status":
                                                     "REJECTED_BOTH"}})}
        self.assertEqual(self.run_check(
            'if [ "$point" = 1 ]; then echo "OK nan nan nan"; '
            'else echo "OK -3.5 1 -2"; fi', ref)[1], "OK")

    def test_a_partly_nan_row_is_not_a_rejection(self):
        ref = {"primary": 0, "points": dict(REF["points"],
                                            **{"1": {"status":
                                                     "REJECTED_BOTH"}})}
        _, status, _, _, _, detail, _ = self.run_check(
            'if [ "$point" = 1 ]; then echo "OK nan 1 nan"; '
            'else echo "OK -3.5 1 -2"; fi', ref)
        self.assertEqual(status, "POINT_NOT_REJECTED")
        self.assertIn("point 1", detail)

    def test_accepting_a_point_cmdstan_rejects_fails(self):
        ref = {"primary": 0, "points": dict(REF["points"],
                                            **{"1": {"status":
                                                     "REJECTED_BOTH"}})}
        _, status, _, _, _, detail, _ = self.run_check(
            'echo "OK -3.5 1 -2"', ref)
        self.assertEqual(status, "POINT_NOT_REJECTED")
        self.assertIn("point 1", detail)

    def test_a_missing_write_array_row_fails_without_a_reference(self):
        # The recorder skips the `wa` block when stanli produced no row, so
        # a model whose write_array is broken records no reference for it.
        # Presence is still demanded: every model has a row.
        _, status, _, _, _, detail, _ = self.run_check(
            'if [ "$point" = 1 ]; then echo "OK -3.5 1 -2"; else '
            'echo "WANAMES a"; echo "WAVALS 1"; echo "OK -3.5 1 -2"; fi',
            wa=False)
        self.assertEqual(status, "WA_FAIL")
        self.assertIn("point 1", detail)

    def test_a_failed_write_array_row_fails_without_a_reference(self):
        _, status, _, _, _, detail, _ = self.run_check(
            'if [ "$point" = 2 ]; then echo "WANAMES FAIL unsupported"; '
            'echo "WAVALS FAIL"; else echo "WANAMES a"; echo "WAVALS 1"; fi\n'
            'echo "OK -3.5 1 -2"', wa=False)
        self.assertEqual(status, "WA_FAIL")
        self.assertIn("point 2", detail)

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

    def test_a_mismatch_point_floors_only_its_named_values(self):
        # The named value is held to 4x the deviation recorded at the
        # point, plus the entry's own floor; the other values keep the
        # clean gate.
        pt = {"values": ["-3.5", "1", "-2"], "status": "MISMATCH",
              "max_rel": 1e-6, "max_ulp": 0}
        ref = {"primary": 0, "points": {str(p): pt for p in POINTS}}
        with unittest.mock.patch.dict(verify_refs.MISMATCH_OUTPUTS, {
                MODEL: verify_refs.Mismatch((1,), "a test")}):
            self.assertEqual(self.run_check('echo "OK -3.5 1.000003 -2"', ref)[1],
                             "OK")
            self.assertEqual(self.run_check('echo "OK -3.5 1.00001 -2"', ref)[1],
                             "GATE")
            self.assertEqual(self.run_check('echo "OK -3.5 1 -2.000003"', ref)[1],
                             "GATE")
        with unittest.mock.patch.dict(verify_refs.MISMATCH_OUTPUTS, {
                MODEL: verify_refs.Mismatch((1,), "a test", floor=1e-4)}):
            self.assertEqual(self.run_check('echo "OK -3.5 1.00001 -2"', ref)[1],
                             "OK")

    def test_an_ill_conditioned_model_floors_only_its_gradient(self):
        # Which of these models' points come out clean is a property of
        # the machine that recorded them, so a clean point in one of them
        # gets the floor too, and the run says the floor carried it.
        body = ('if [ "$point" = 2 ]; then echo "OK -3.5 1 -2.000001"; '
                'else echo "OK -3.5 1 -2"; fi')
        self.assertEqual(self.run_check(body)[1], "GATE")
        with unittest.mock.patch.dict(ILL_CONDITIONED, {MODEL: "test"}):
            _, status, _, _, _, _, notes = self.run_check(body)
            self.assertEqual(status, "OK")
            self.assertIn(f"FLOORED {MODEL} point 2: 5.00e-07", " ".join(notes))
            self.assertNotIn("point 1", " ".join(notes))
            # 5e-6 is under the old 3.3% floor and over the measured one.
            self.assertEqual(self.run_check('echo "OK -3.5 1 -2.00001"')[1],
                             "GATE")
            # lp keeps the clean gate.
            self.assertEqual(self.run_check('echo "OK -3.50001 1 -2"')[1],
                             "GATE")

    def test_an_ill_conditioned_log_density_keeps_its_own_limit(self):
        lp_off = 'echo "OK -3.50000001 1 -2"'  # 2.9e-9 relative
        with unittest.mock.patch.dict(ILL_CONDITIONED, {MODEL: "test"}):
            self.assertEqual(self.run_check(lp_off)[1], "GATE")
            with unittest.mock.patch.dict(verify_refs.ILL_CONDITIONED_LP_GATE,
                                          {MODEL: 5e-8}):
                self.assertEqual(self.run_check(lp_off)[1], "OK")
                result = self.run_check('echo "OK -3.5000002 1 -2"')
                self.assertEqual(result[1], "GATE")
                self.assertIn("at lp, allowed 5.0e-08", result[5])
                # The gradient keeps the ill-conditioned gradient gate.
                self.assertEqual(
                    self.run_check('echo "OK -3.5 1 -2.000001"')[1], "OK")
                self.assertEqual(
                    self.run_check('echo "OK -3.5 1 -2.00001"')[1], "GATE")

    def test_a_known_gap_may_refuse_the_model(self):
        gap = verify_refs.Gap(verify_refs.WHOLE_MODEL, "test gap")
        with unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: gap}):
            _, status, _, _, _, _, notes = self.run_check(
                'echo "COMPILE_FAIL unsupported function"')
        self.assertEqual(status, "OK")
        self.assertIn(f"KNOWN GAP {MODEL}: test gap", notes)

    def test_a_known_gap_that_passes_is_reported(self):
        for excuses in (verify_refs.WHOLE_MODEL, verify_refs.WRITE_ARRAY):
            gap = verify_refs.Gap(excuses, "test gap")
            with self.subTest(excuses=excuses), \
                    unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: gap}):
                _, status, _, _, _, detail, _ = self.run_check(
                    'echo "OK -3.5 1 -2"')
                self.assertEqual(status, "GAP_CLOSED")
                self.assertIn(MODEL, detail)

    def test_a_known_gap_still_may_not_crash(self):
        for excuses in (verify_refs.WHOLE_MODEL, verify_refs.WRITE_ARRAY):
            gap = verify_refs.Gap(excuses, "test gap")
            with self.subTest(excuses=excuses), \
                    unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: gap}):
                _, status, _, _, _, detail, _ = self.run_check('kill -SEGV $$')
                self.assertEqual(status, "CRASH")
                self.assertIn("signal 11", detail)

    # What stanli_check reports for a model whose generated quantities
    # call a function that only the interpreter knows, and it throws.
    WA_GAP = (compiled(["generated quantities (unsupported f)"], "interpreted",
                       "generated quantities (unsupported f)")
              + 'if [ -n "$wa" ]; then echo "WANAMES FAIL unsupported f"; '
                'echo "WAVALS FAIL"; fi\n')

    def test_a_write_array_gap_excuses_only_the_write_array(self):
        gap = verify_refs.Gap(verify_refs.WRITE_ARRAY, "test gap")
        with unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: gap}):
            _, status, _, _, _, _, notes = self.run_check(
                self.WA_GAP + 'echo "OK -3.5 1 -2"', wa=False)
            self.assertEqual(status, "OK")
            self.assertIn(f"KNOWN GAP {MODEL}: test gap", notes)
            # lp and the gradient are still compared, at every point.
            for body in ('echo "OK -3.6 1 -2"',
                         'if [ "$point" = 2 ]; then echo "OK -3.5 1 -2.1"; '
                         'else echo "OK -3.5 1 -2"; fi'):
                with self.subTest(body=body):
                    self.assertEqual(self.run_check(self.WA_GAP + body,
                                                    wa=False)[1], "GATE")
            # So is the ULP gate.
            near = 1 + 40 * math.ulp(1.0)
            with tempfile.TemporaryDirectory() as tmp:
                result = check_model(
                    MODEL, self.REF, REPO / "nonexistent-pdb",
                    stub(tmp, self.WA_GAP + f'echo "OK -3.5 {near!r} -2"', False),
                    pathlib.Path(tmp), 60, 1e-9, ulp_entries={})
            self.assertEqual(result[1], "ULP_GATE")
            # A log density that runs through the interpreter is not part
            # of the gap.
            other = compiled(["generated quantities (unsupported f)",
                              "the ODE right-hand side g (unsupported h)"],
                             "interpreted", "generated quantities (unsupported f)")
            body = (other + 'if [ -n "$wa" ]; then echo "WANAMES FAIL x"; '
                    'echo "WAVALS FAIL"; fi\necho "OK -3.5 1 -2"')
            self.assertEqual(self.run_check(body, wa=False)[1], "INTERPRETER")
            # Neither is a model that stanli refuses.
            self.assertEqual(
                self.run_check('echo "COMPILE_FAIL unsupported function"')[1],
                "RUN_FAIL")

    def test_a_gap_says_what_it_excuses(self):
        with self.assertRaises(ValueError):
            verify_refs.Gap("generated quantities", "test gap")
        for gap in KNOWN_GAPS.values():
            self.assertIn(gap.excuses, (verify_refs.WRITE_ARRAY,
                                        verify_refs.WHOLE_MODEL))


class PairDevTest(unittest.TestCase):
    """What the scaled-error gate accepts, and what the ULP gate adds."""

    def test_values_below_one_are_held_to_an_absolute_error(self):
        rel, ulp = verify_refs.pair_dev(1e-4, 1e-4 + 9e-10)
        self.assertAlmostEqual(rel, 9e-10, delta=1e-20)
        self.assertGreater(ulp, 6e10)
        self.assertLess(rel, 1e-9)  # so the scaled-error gate passes it
        rel, _ = verify_refs.pair_dev(1e-12, -1e-12)
        self.assertLess(rel, 1e-9)  # a sign error passes it too

    def test_large_values_are_relative(self):
        self.assertEqual(verify_refs.pair_dev(1e4, 1e4 * (1 + 1e-8))[0] > 1e-9,
                         True)

    def test_nonfinite_values(self):
        nan, inf = float("nan"), float("inf")
        self.assertEqual(verify_refs.pair_dev(nan, nan), (0.0, 0))
        self.assertEqual(verify_refs.pair_dev(inf, inf), (0.0, 0))
        self.assertEqual(verify_refs.pair_dev(inf, 1.0)[0], math.inf)
        self.assertEqual(verify_refs.pair_dev(nan, 1.0)[0], math.inf)


class ScaledUlpTest(unittest.TestCase):

    def test_the_audit_cases_fail(self):
        self.assertGreater(verify_refs.scaled_ulp(1e-4, 1e-4 + 9e-10, 1e-4),
                           6e10)
        self.assertEqual(verify_refs.scaled_ulp(1e-12, -1e-12, 1.0),
                         math.ceil(2e-12 / math.ulp(1.0)))
        self.assertGreater(verify_refs.scaled_ulp(1e-12, -1e-12, 1.0),
                           verify_refs.DEFAULT_ULP_BOUND)

    def test_a_cancelled_entry_is_counted_in_ulps_of_the_scale(self):
        # A gradient entry that CmdStan cancels to 0 and stanli to one
        # rounding of a term of size 0.005.
        self.assertGreater(verify_refs.ulp_distance(0.0, 1.1102230246251566e-18),
                           1e18)
        self.assertEqual(verify_refs.scaled_ulp(0.0, 1.1102230246251566e-18,
                                                0.005), 2)

    def test_the_element_distance_is_used_when_it_is_smaller(self):
        a = 1e-3
        b = math.nextafter(math.nextafter(a, 1), 1)
        self.assertEqual(verify_refs.scaled_ulp(a, b, 0.0), 2)
        self.assertEqual(verify_refs.scaled_ulp(a, b, 1e-3), 2)

    def test_nonfinite_values(self):
        nan, inf = float("nan"), float("inf")
        self.assertEqual(verify_refs.scaled_ulp(nan, nan, 1.0), 0)
        self.assertEqual(verify_refs.scaled_ulp(-inf, -inf, 1.0), 0)
        self.assertEqual(verify_refs.scaled_ulp(inf, 1.0, 1.0), math.inf)
        self.assertEqual(verify_refs.scaled_ulp(nan, 1.0, 1.0), math.inf)

    def test_the_gradient_scale_comes_from_cmdstan(self):
        # stanli cannot widen its own floor with a wrong large value.
        ulps = verify_refs.density_ulps([-3.5, 1e-4, 1e-3], [-3.5, 1e-4, 1e3])
        self.assertGreater(ulps["grad[2]"], 1e10)
        ulps = verify_refs.density_ulps([-3.5, 1000.0, 0.0],
                                        [-3.5, 1000.0, 5 * math.ulp(1000.0)])
        self.assertEqual(ulps, {"grad[2]": 5})

    def test_lp_is_scored_on_its_own_magnitude(self):
        lp = 1e-6
        ulps = verify_refs.density_ulps([lp, 1e6], [lp * (1 + 1e-12), 1e6])
        self.assertGreater(ulps["lp"], 1000)

    def test_write_array_columns_are_scored_within_their_variable(self):
        names = ["mu", "theta.1", "theta.2", "z.1"]
        ref = [100.0, 1.0, 0.0, 0.0]
        got = [100.0, 1.0, 2 * math.ulp(1.0), 2 * math.ulp(1.0)]
        ulps = verify_refs.write_array_ulps(names, ref, got)
        self.assertEqual(ulps["wa theta.2"], 2)
        # z has no other entry, so its zero gets no floor.
        self.assertGreater(ulps["wa z.1"], 1e18)
        self.assertNotIn("wa mu", ulps)


class UlpLedgerTest(unittest.TestCase):
    """The per-model exceptions to the ULP gate, and how they move."""

    ENTRIES = {"lp": {"bound": 40, "reason": "lp test"},
               "grad": {"bound": 30, "reason": "kind test"},
               "grad[2]": {"bound": 500, "reason": "value test"}}

    def test_a_value_takes_its_own_entry_then_its_kind_then_the_default(self):
        bound = verify_refs.ulp_bound
        self.assertEqual(bound(self.ENTRIES, "grad[2]"), (500, "value test"))
        self.assertEqual(bound(self.ENTRIES, "grad[7]"), (30, "kind test"))
        self.assertEqual(bound(self.ENTRIES, "lp"), (40, "lp test"))
        self.assertEqual(bound(self.ENTRIES, "wa mu"),
                         (verify_refs.DEFAULT_ULP_BOUND, "default"))
        self.assertEqual(bound({}, "grad[1]")[0], verify_refs.DEFAULT_ULP_BOUND)

    def test_the_gate_names_the_value_and_the_reason(self):
        failed = verify_refs.ulp_gate(self.ENTRIES, {"grad[7]": 31}, 1,
                                      {"grad[7]": (1.0, 1.5)})
        self.assertEqual(failed[0], "ULP_GATE")
        self.assertIn("point 1: grad[7] (stanli 1.5, CmdStan 1.0) is 31", failed[1])
        self.assertIn("allowed 30 (kind test)", failed[1])
        self.assertIsNone(verify_refs.ulp_gate(self.ENTRIES, {"grad[2]": 500}, 0))
        self.assertIsNone(verify_refs.ulp_gate({}, {"grad[2]": 10}, 0))
        self.assertEqual(verify_refs.ulp_gate({}, {"grad[2]": 11}, 0)[0],
                         "ULP_GATE")

    def test_entries_only_go_down(self):
        new = verify_refs.updated_entries(
            self.ENTRIES, {"lp": 12, "grad[2]": 9, "grad[5]": 25}, "new")
        self.assertEqual(new, {"lp": {"bound": 12, "reason": "lp test"},
                               "grad": {"bound": 25, "reason": "kind test"}})
        with self.assertRaises(ValueError):
            verify_refs.updated_entries(self.ENTRIES, {"lp": 41}, "new")

    def test_new_values_get_generated_entries(self):
        measured = {"lp": 518, "grad[1]": 11, "grad[3]": 10, "wa mu": 12}
        new = verify_refs.updated_entries({}, measured, "measured here")
        self.assertEqual(new, {
            "lp": {"bound": 518, "reason": "measured here"},
            "grad[1]": {"bound": 11, "reason": "measured here"},
            "wa mu": {"bound": 12, "reason": "measured here"}})
        many = {f"grad[{i}]": 20 + i for i in range(1, 11)}
        new = verify_refs.updated_entries({}, many, "measured here")
        self.assertEqual(list(new), ["grad"])
        self.assertEqual(new["grad"]["bound"], 30)
        self.assertIn("10 grad values over 10", new["grad"]["reason"])

    def test_a_wide_margin_is_reported(self):
        notes = verify_refs.ledger_notes("m", self.ENTRIES,
                                         {"lp": 39, "grad[2]": 250, "grad[4]": 3})
        self.assertEqual(len(notes), 2)
        self.assertIn("ULP LEDGER m grad: measured 3, recorded bound 30", notes[0])
        self.assertIn("ULP LEDGER m grad[2]: measured 250", notes[1])

    def test_the_ledger_file_is_validated(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "ledger.json"
            for entry in ({"bound": 10, "reason": "x"}, {"bound": 11},
                          {"bound": 11.5, "reason": "x"}):
                path.write_text(json.dumps({"schema": 1, "platforms": {
                    "P": {"main": {"m": {"lp": entry}}}}}))
                with self.subTest(entry=entry), self.assertRaises(SystemExit):
                    verify_refs.load_ulp_ledger(path)
            path.unlink()
            empty = verify_refs.load_ulp_ledger(path)
            self.assertIsNone(verify_refs.ulp_section(empty, "P", "main"))

    def test_writing_a_section_ratchets_only_the_models_that_ran(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "ledger.json"
            ledger = {"schema": 1, "platforms": {"P": {"main": {
                "kept": {"lp": {"bound": 99, "reason": "not run"}},
                "ran": {"lp": {"bound": 99, "reason": "why"}}}}}}
            verify_refs.write_ulp_ledger(
                ledger, "P", "main", {"ran": {"lp": 50, "grad[1]": 20},
                                      "clean": {"lp": 3}},
                ["ran", "clean"], {"ran": "Q"}, path)
            written = verify_refs.load_ulp_ledger(path)
            section = verify_refs.ulp_section(written, "P", "main")
            self.assertEqual(section["kept"]["lp"]["bound"], 99)
            self.assertEqual(section["ran"]["lp"], {"bound": 50, "reason": "why"})
            self.assertIn("against a reference recorded on Q",
                          section["ran"]["grad[1]"]["reason"])
            self.assertNotIn("clean", section)

    def test_first_entries_need_a_reason_for_every_value(self):
        measured = {"lp": 518, "grad[1]": 11, "grad[3]": 10}
        reasons = {"lp": "lp why", "grad[1]": "grad why"}
        entries, problems = verify_refs.first_entries(measured, reasons, "P")
        self.assertEqual(problems, [])
        self.assertEqual(entries, {
            "lp": {"bound": 518, "reason": "measured on P; lp why"},
            "grad[1]": {"bound": 11, "reason": "measured on P; grad why"}})
        _, problems = verify_refs.first_entries(measured, {"lp": "why"}, "P")
        self.assertEqual(problems, ["grad[1] measured 11 needs a reason"])
        _, problems = verify_refs.first_entries(
            measured, {**reasons, "grad[3]": "why"}, "P")
        self.assertEqual(problems, ["grad[3] has a reason but is within 10"])
        many = {f"grad[{i}]": 20 + i for i in range(1, 11)}
        entries, problems = verify_refs.first_entries(many, {"grad": "why"}, "P")
        self.assertEqual(problems, [])
        self.assertEqual(entries["grad"], {
            "bound": 30, "reason": "measured on P; why; 10 grad values over 10"})

    def test_named_entries_replace_a_model_and_keep_unnamed_reasons(self):
        old = {"lp": {"bound": 20, "reason": "lp before"},
               "grad[1]": {"bound": 15, "reason": "grad before"}}
        measured = {"lp": 12, "grad[1]": 30, "grad[2]": 25}
        entries, problems = verify_refs.first_entries(
            measured, {"grad[1]": "moved", "grad[2]": "new"}, "P", old)
        self.assertEqual(problems, [])
        self.assertEqual(entries, {
            "lp": {"bound": 12, "reason": "lp before"},
            "grad[1]": {"bound": 30, "reason": "measured on P; moved"},
            "grad[2]": {"bound": 25, "reason": "measured on P; new"}})
        _, problems = verify_refs.first_entries(measured, {"grad[2]": "new"},
                                                "P", old)
        self.assertEqual(problems, ["grad[1] measured 30 needs a reason"])

    def test_entries_may_be_added_to_models_that_have_some(self):
        section = {"old": {"lp": {"bound": 20, "reason": "why"}}}
        check = verify_refs.first_entry_models
        self.assertEqual(check({"new": {}}, section, ["new", "old"]), [])
        self.assertEqual(check({"old": {"lp": "why"}}, section, ["old"]), [])
        self.assertEqual(check({"new": {}}, section, ["old"]),
                         ["new is not in this replay"])
        self.assertEqual(check({"new": {}}, None, ["new"]),
                         ["the ledger has no section here; record it with "
                          "--update-ulp-ledger"])

    def test_adding_first_entries_writes_only_the_named_models(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "ledger.json"
            ledger = {"schema": 1, "platforms": {"P": {"main": {
                "old": {"lp": {"bound": 99, "reason": "why"}}}}}}
            problems = verify_refs.add_first_entries(
                ledger, "P", "main",
                {"old": {"lp": 50}, "new": {"lp": 30, "grad[1]": 3},
                 "clean": {}},
                {"new": {"lp": "new why"}, "clean": {}}, path)
            self.assertEqual(problems, [])
            section = verify_refs.ulp_section(verify_refs.load_ulp_ledger(path),
                                              "P", "main")
            self.assertEqual(section, {
                "new": {"lp": {"bound": 30, "reason": "measured on P; new why"}},
                "old": {"lp": {"bound": 99, "reason": "why"}}})
            path.unlink()
            problems = verify_refs.add_first_entries(
                ledger, "P", "main", {"new": {"lp": 31}}, {"new": {}}, path)
            self.assertEqual(problems, ["new: lp measured 31 needs a reason"])
            self.assertFalse(path.exists())

    def test_the_checked_in_ledger_loads(self):
        ledger = verify_refs.load_ulp_ledger()
        self.assertIsNotNone(ledger["platforms"])


class UlpGateReplayTest(unittest.TestCase):
    """check_model with a ULP ledger section, as the replays run it."""

    PT = {"values": ["-3.5", "1e-4", "2"], "status": "VERIFIED", "max_rel": 0.0}
    REF = {"primary": 0, "points": dict.fromkeys(map(str, POINTS), PT)}

    def run_check(self, body, entries, measured=None):
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(MODEL, self.REF, REPO / "nonexistent-pdb",
                               stub(tmp, body), pathlib.Path(tmp), 60, 1e-9,
                               ulp_entries=entries, measured=measured)

    def test_an_absolute_error_below_1e9_fails_the_ulp_gate(self):
        body = f'echo "OK -3.5 {1e-4 + 9e-10!r} 2"'
        self.assertEqual(self.run_check(body, None)[1], "OK")
        result = self.run_check(body, {})
        self.assertEqual(result[1], "ULP_GATE")
        self.assertIn("grad[1]", result[5])
        self.assertIn("allowed 10 (default)", result[5])

    def test_a_ledger_entry_allows_its_measured_bound(self):
        near = 2 + 40 * math.ulp(2.0)
        body = f'echo "OK -3.5 1e-4 {near!r}"'
        self.assertEqual(self.run_check(body, {})[1], "ULP_GATE")
        measured = {}
        result = self.run_check(body, {"grad[2]": {"bound": 40, "reason": "r"}},
                                measured)
        self.assertEqual(result[1], "OK")
        self.assertEqual(measured, {"grad[2]": 40})
        result = self.run_check('echo "OK -3.5 1e-4 2"',
                                {"grad[2]": {"bound": 40, "reason": "r"}})
        self.assertEqual(result[1], "OK")
        self.assertIn("ULP LEDGER while_counter_size grad[2]: measured 0",
                      " ".join(result[6]))

    def test_write_array_values_are_gated(self):
        body = ('echo "OK -3.5 1e-4 2"\n'
                'if [ -n "$wa" ]; then echo "WANAMES a"; '
                f'echo "WAVALS {1e-4 + 1e-12!r}"; fi')
        ref = {"primary": 0, "points": {str(p): {
            **self.PT, "wa": {"names": "a", "values": ["1e-4"]}} for p in POINTS}}
        with tempfile.TemporaryDirectory() as tmp:
            result = check_model(MODEL, ref, REPO / "nonexistent-pdb",
                                 stub(tmp, body, wa=False), pathlib.Path(tmp),
                                 60, 1e-9, ulp_entries={})
        self.assertEqual(result[1], "ULP_GATE")
        self.assertIn("wa a", result[5])


class FastModeGateTest(unittest.TestCase):
    """--fast-math: lp and the gradient as a vector, not per coordinate."""

    WIDE = {"values": ["-3.5", "1000", "1e-13"], "status": "VERIFIED",
            "max_rel": 0.0, "max_ulp": 0}

    def run_check(self, body, values=None, fast=True, wa=True):
        pt = values or VALUES
        ref = {"primary": 0, "points": {str(p): pt for p in POINTS},
               "recorded": {"platform": verify_refs.native_platform()}}
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(MODEL, ref, REPO / "nonexistent-pdb",
                               stub(tmp, body, wa), pathlib.Path(tmp), 60,
                               verify_refs.FAST_MAX_REL if fast else 1e-9,
                               fast_math=fast)

    def test_the_flag_reaches_stanli_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "stanli_check_stub.sh"
            path.write_text('#!/bin/sh\nstatus="$(eval echo \\${$#})"\n'
                            + compiled() +
                            'case " $* " in\n'
                            '  *" --fast-math "*) echo "OK -3.5 1 -2";;\n'
                            '  *) echo "COMPILE_FAIL default mode";;\nesac\n')
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
            ref = {"primary": 0, "points": {str(p): VALUES for p in POINTS}}
            args = (MODEL, ref, REPO / "nonexistent-pdb", path,
                    pathlib.Path(tmp), 60, 1e-12)
            self.assertEqual(check_model(*args, no_wa=True,
                                         fast_math=True)[1], "OK")
            self.assertNotEqual(check_model(*args, no_wa=True)[1], "OK")

    def test_a_cancelled_coordinate_is_scored_against_the_gradient(self):
        # 2e-13 away in a coordinate of 1e-13: thousands of ULP there, and
        # 2e-16 of the largest entry.
        body = 'echo "OK -3.5 1000 3e-13"'
        result = self.run_check(body, self.WIDE)
        self.assertEqual(result[1], "OK")
        self.assertLess(result[2], 1e-15)
        self.assertGreater(result[3], 1000)
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            self.assertEqual(self.run_check(body, self.WIDE)[1], "OK")
            self.assertEqual(self.run_check(body, self.WIDE, fast=False)[1],
                             "ULP_GATE")

    def test_the_gradient_gate_is_tighter_than_the_default(self):
        body = 'echo "OK -3.5 1000.00000001 1e-13"'  # 1e-11 of the largest
        self.assertEqual(self.run_check(body, self.WIDE)[1], "GATE")
        self.assertEqual(self.run_check(body, self.WIDE, fast=False)[1], "OK")

    def test_lp_is_gated_on_its_own(self):
        self.assertEqual(
            self.run_check('echo "OK -3.50000000004 1 -2"')[1], "GATE")
        self.assertEqual(
            self.run_check('echo "OK -3.5000000000000004 1 -2"')[1], "OK")

    def test_a_nonfinite_coordinate_fails(self):
        for got in ("nan", "inf"):
            with self.subTest(got=got):
                self.assertEqual(
                    self.run_check(f'echo "OK -3.5 1 {got}"')[1], "GATE")

    def test_a_point_cmdstan_rejects_must_still_be_rejected(self):
        rejected = {"status": "REJECTED_BOTH"}
        self.assertEqual(
            self.run_check('echo "EVAL_FAIL out of range"', rejected)[1], "OK")
        self.assertEqual(
            self.run_check('echo "OK -3.5 1 -2"', rejected)[1],
            "POINT_NOT_REJECTED")

    def test_ill_conditioned_models_keep_their_documented_limit(self):
        # A point with no recorded deviation, as the brms GP points are.
        pt = {"values": VALUES["values"], "status": "VERIFIED", "max_rel": 0.0}
        body = 'echo "OK -3.5 1.0000000001 -2"'  # 1e-10 of the largest
        self.assertEqual(self.run_check(body, pt)[1], "GATE")
        with unittest.mock.patch.dict(verify_refs.ILL_CONDITIONED,
                                      {MODEL: "a test"}):
            self.assertEqual(self.run_check(body, pt)[1], "OK")
            self.assertEqual(
                self.run_check('echo "OK -3.5 1.00001 -2"', pt)[1], "GATE")
            self.assertEqual(
                self.run_check('echo "OK -3.50001 1 -2"', pt)[1], "GATE")

    def test_other_outputs_keep_the_default_gate(self):
        pt = {**VALUES, "wa": {"names": "a", "values": ["1"]}}
        row = ('if [ -n "$wa" ]; then echo "WANAMES a"; '
               'echo "WAVALS %s"; fi\necho "OK -3.5 1 -2"')
        # 1e-10 is above the fast gate and below the one these outputs keep.
        near = self.run_check(row % "1.0000000001", pt, wa=False)
        self.assertEqual(near[1], "OK")
        self.assertEqual(near[2], 0.0)
        self.assertEqual(
            self.run_check(row % "1.00000001", pt, wa=False)[1], "WA_GATE")


class CorpusOptionsTest(unittest.TestCase):
    """The per-corpus gates CTest replays local corpora under."""

    SOURCE = REPO / "tests" / "regressions" / f"{MODEL}.stan"
    WA = {**VALUES, "wa": {"names": "a,b", "values": ["1", "2"]}}

    def run_check(self, body, pt=None, wa=True, **options):
        ref = {"points": {str(p): pt or self.WA for p in POINTS}}
        opts = verify_refs.Replay(self.SOURCE, self.SOURCE.with_suffix(".json"),
                                  **options)
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(MODEL, ref, None, stub(tmp, body, wa),
                               pathlib.Path(tmp), 60, 1e-9, opts=opts)

    def row(self, values, extra=""):
        return ('if [ -n "$wa" ]; then echo "WANAMES a,b"; '
                f'echo "WAVALS {values}"; fi\n{extra}echo "OK -3.5 1 -2"')

    def test_permuted_outputs_fail_and_name_the_column(self):
        self.assertEqual(self.run_check(self.row("1 2"), wa=False)[1], "OK")
        result = self.run_check(self.row("2 1"), wa=False)
        self.assertEqual(result[1], "GATE")
        self.assertIn("worst at a", result[5])

    def test_a_gradient_failure_names_its_case(self):
        result = self.run_check('echo "OK -3.5 1 -2.5"', VALUES,
                                grad_labels=("f(real)=>real", "g(real)=>real"))
        self.assertEqual(result[1], "GATE")
        self.assertIn("grad[2] g(real)=>real", result[5])

    def test_interpreter_use_must_match_the_expectation(self):
        interp = compiled(["transformed parameters"])
        self.assertEqual(self.run_check(self.row("1 2", interp), wa=False)[1],
                         "INTERPRETER")
        self.assertEqual(self.run_check(self.row("1 2"), wa=False,
                                        interpreted=True)[1], "INTERPRETER")
        self.assertEqual(self.run_check(self.row("1 2", interp), wa=False,
                                        interpreted=True)[1], "OK")
        wa_interp = compiled(section="interpreted")
        self.assertEqual(self.run_check(self.row("1 2", wa_interp),
                                        wa=False)[1], "INTERPRETER")
        note = 'echo "write_array interpreted" >&2\n'
        self.assertEqual(self.run_check(self.row("1 2", note), wa=False)[1],
                         "OK")

    def test_a_run_that_writes_no_compile_record_is_a_crash(self):
        result = self.run_check(self.row("1 2", 'rm -f "$status"\n'),
                                wa=False)
        self.assertEqual(result[1], "CRASH")
        self.assertIn("no status record", result[5])

    def test_the_scale_floor_holds_small_values_to_an_absolute_error(self):
        pt = {"values": ["-3.5", "1e-6", "-2"]}
        body = 'echo "OK -3.5 1.00001e-06 -2"'
        self.assertEqual(self.run_check(body, pt)[1], "OK")
        self.assertEqual(self.run_check(body, pt, scale_floor=1e-3)[1], "GATE")

    def test_the_ulp_limit_applies_on_every_platform(self):
        body = f'echo "OK -3.5 {1.0 + 11 * math.ulp(1.0)!r} -2"'
        self.assertEqual(self.run_check(body, VALUES)[1], "OK")
        result = self.run_check(body, VALUES, ulp=10)
        self.assertEqual(result[1], "ULP_GATE")
        self.assertIn("grad[1] (stanli 1.0000000000000024, CmdStan 1.0)",
                      result[5])

    def test_cancelling_fixtures_use_the_largest_entry_off_platform(self):
        # 11 ULP of a gradient entry of 1, 1 ULP of the largest entry (8).
        pt = {"values": ["-3.5", "1", "8"]}
        body = f'echo "OK -3.5 {1.0 + 11 * math.ulp(1.0)!r} 8"'
        listed = {MODEL: "cause"}

        def run(platform):
            ref = {"recorded": {"platform": platform},
                   "points": {str(p): pt for p in POINTS}}
            opts = verify_refs.Replay(self.SOURCE,
                                      self.SOURCE.with_suffix(".json"), ulp=10)
            with tempfile.TemporaryDirectory() as tmp, unittest.mock.patch.dict(
                    verify_refs.GRADIENT_AGAINST_LARGEST_OFF_PLATFORM, listed):
                return check_model(MODEL, ref, None, stub(tmp, body),
                                   pathlib.Path(tmp), 60, 1e-9, opts=opts)[1]

        here = verify_refs.native_platform()
        self.assertEqual(run(here), "ULP_GATE")
        self.assertEqual(run("Elsewhere x"), "OK")
        listed.clear()
        self.assertEqual(run("Elsewhere x"), "ULP_GATE")

    def test_listed_write_array_variables_use_their_largest_entry_off_platform(self):
        # m.1 is 16 ULP from 0.25 and 4 ULP of m's largest entry, 1; s is a
        # scalar with no larger entry to be measured against.
        near = 0.25 + 16 * math.ulp(0.25)
        pt = {**VALUES, "wa": {"names": "m.1,m.2,s",
                               "values": ["0.25", "1", "0.25"]}}
        listed = {MODEL: (("m",), "cause")}

        def run(platform, s=0.25):
            ref = {"recorded": {"platform": platform},
                   "points": {str(p): pt for p in POINTS}}
            body = ('if [ -n "$wa" ]; then echo "WANAMES m.1,m.2,s"; '
                    f'echo "WAVALS {near!r} 1 {s!r}"; fi\necho "OK -3.5 1 -2"')
            opts = verify_refs.Replay(self.SOURCE,
                                      self.SOURCE.with_suffix(".json"), ulp=10)
            with tempfile.TemporaryDirectory() as tmp, unittest.mock.patch.dict(
                    verify_refs.WRITE_ARRAY_AGAINST_LARGEST_OFF_PLATFORM, listed):
                return check_model(MODEL, ref, None, stub(tmp, body, wa=False),
                                   pathlib.Path(tmp), 60, 1e-9, opts=opts)[1]

        self.assertEqual(run(verify_refs.native_platform()), "ULP_GATE")
        self.assertEqual(run("Elsewhere x"), "OK")
        self.assertEqual(run("Elsewhere x", s=near), "ULP_GATE")
        listed.clear()
        self.assertEqual(run("Elsewhere x"), "ULP_GATE")

    def test_a_wider_value_limit_needs_its_independent_value(self):
        near = 1.0 + 12 * math.ulp(1.0)
        body = f'echo "OK -3.5 {near!r} -2"'
        limits = {1: (16, "a test", lambda outputs: near)}
        self.assertEqual(self.run_check(body, VALUES, ulp=10)[1], "ULP_GATE")
        self.assertEqual(self.run_check(body, VALUES, ulp=10,
                                        value_limits=limits)[1], "OK")
        far = {1: (16, "a test", lambda outputs: 1.0)}
        self.assertEqual(self.run_check(body, VALUES, ulp=10,
                                        value_limits=far)[1], "ULP_GATE")
        wide = f'echo "OK -3.5 {1.0 + 17 * math.ulp(1.0)!r} -2"'
        self.assertEqual(self.run_check(wide, VALUES, ulp=10,
                                        value_limits=limits)[1], "ULP_GATE")


class LocalCorpusTest(unittest.TestCase):
    """Models carried in the tree, resolved by name before posteriordb."""

    def test_every_local_model_is_found_where_it_lives(self):
        for name, case in local_cases().items():
            found, data = model_files(name, {}, REPO / "nonexistent",
                                      REPO / "nonexistent")
            self.assertEqual((found, data), (case.source, case.data))


class SharedHeaderWorkTest(unittest.TestCase):
    def test_equal_source_basenames_use_separate_model_build_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with unittest.mock.patch("verify_refs.tempfile.mkdtemp", return_value=tmp), \
                 unittest.mock.patch("verify_refs.corpus_models", return_value=[("a", None), ("b", None)]), \
                 unittest.mock.patch("verify_refs.corpus_input", return_value=(root / "model.stan", root / "data.json")), \
                 unittest.mock.patch("verify_refs.subprocess.run", side_effect=WriteArrayHeaderTest.columns(None)), \
                 unittest.mock.patch("verify_refs.cmdstan_header", return_value=(["theta"], "")) as header:
                self.assertEqual(verify_refs.check_wa_headers(root, root / "check", root / "cmdstan", []), 0)
            self.assertEqual([call.args[1] for call in header.call_args_list], [root / "a", root / "b"])


class WriteArrayHeaderTest(unittest.TestCase):
    """--wa-headers compares a truncated graph's columns as a prefix."""

    @staticmethod
    def columns(truncated):
        """A subprocess.run for stanli_check --columns printing theta."""
        def run(argv, **_):
            status = pathlib.Path(argv[argv.index("--status-json") + 1])
            status.write_text(json.dumps(
                {"record": "compile", "ok": True, "interpreter_fallbacks": [],
                 "write_array": {"section": "graph", "columns": 1,
                                 "truncated": truncated}}) + "\n")
            return unittest.mock.Mock(returncode=0, stdout="theta", stderr="")
        return run

    def headers(self, truncated):
        run = self.columns(truncated)
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with unittest.mock.patch("verify_refs.tempfile.mkdtemp", return_value=tmp), \
                 unittest.mock.patch("verify_refs.corpus_models", return_value=[("a", None)]), \
                 unittest.mock.patch("verify_refs.corpus_input", return_value=(root / "model.stan", root / "data.json")), \
                 unittest.mock.patch("verify_refs.subprocess.run", side_effect=run), \
                 unittest.mock.patch("verify_refs.cmdstan_header", return_value=(["theta", "y"], "")):
                return verify_refs.check_wa_headers(root, root / "check", root / "cmdstan", [])

    def test_a_truncated_graph_matches_a_prefix(self):
        self.assertEqual(self.headers("unsupported function f"), 0)

    def test_a_complete_graph_must_match_every_column(self):
        self.assertEqual(self.headers(None), 1)


class WriteArrayCoverageTest(unittest.TestCase):
    """--wa-report reads the write_array outcome from the status records."""

    def coverage(self, section, truncated=None, wa=None):
        compile_ = {"record": "compile", "ok": True,
                    "interpreter_fallbacks": [],
                    "write_array": {"section": section, "columns": 2,
                                    "truncated": truncated}}
        point = {"record": "point", "point": 0, "ok": True}
        if wa is not None:
            point["write_array"] = wa
        return verify_refs.wa_coverage(
            check_status.CheckStatus(compile_, {0: point}), 0)

    def test_each_outcome(self):
        done = {"ok": True, "columns": 2, "values": 3, "nonfinite": 1}
        self.assertEqual(self.coverage("none")[:2], ("NO_GQ_SECTION", ""))
        self.assertEqual(self.coverage("graph", wa=done),
                         ("COMPLETE", "2 vars, 1/3 nonfinite", None))
        self.assertEqual(self.coverage("interpreted", "f", done)[:2],
                         ("COMPLETE", "2 vars (interpreted), 1/3 nonfinite"))
        self.assertEqual(self.coverage("graph", "unsupported function f",
                                       done),
                         ("TRUNCATED", "unsupported function f, 1/3 nonfinite",
                          "unsupported function f"))
        self.assertEqual(self.coverage("graph", "unsupported function f"),
                         ("EMPTY", "unsupported function f",
                          "unsupported function f"))
        threw = {"ok": False, "error": "domain_error", "message": "bad"}
        self.assertEqual(self.coverage("graph", wa=threw),
                         ("EMPTY", "bad", "bad"))


class StrictReferenceTest(unittest.TestCase):
    """Full-output fixtures retain their stronger oracle after consolidation."""

    REF = {**REF, "strict": True,
           "points": {str(p): {**VALUES, "wa": {"names": "a", "values": ["1"]}}
                      for p in POINTS}}

    run_check = CheckModelPointsTest.run_check

    def test_missing_point_fails_instead_of_probing(self):
        ref = copy.deepcopy(self.REF)
        del ref["points"]["1"]
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"', ref)[1],
                         "REFERENCE_INCOMPLETE")

    def test_missing_output_reference_fails(self):
        ref = copy.deepcopy(self.REF)
        del ref["points"]["1"]["wa"]
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"', ref)[1],
                         "REFERENCE_INCOMPLETE")

    def test_matching_nonfinite_values_still_fail(self):
        ref = copy.deepcopy(self.REF)
        for pt in ref["points"].values():
            pt["values"] = ["-3.5", "nan", "-2"]
        self.assertEqual(self.run_check('echo "OK -3.5 nan -2"', ref)[1], "NONFINITE")

    def test_matching_nonfinite_outputs_still_fail(self):
        ref = copy.deepcopy(self.REF)
        for pt in ref["points"].values():
            pt["wa"]["values"] = ["nan"]
        result = self.run_check('echo "WANAMES a"; echo "WAVALS nan"; echo "OK -3.5 1 -2"',
                                ref, wa=False)
        self.assertEqual(result[1], "WA_NONFINITE")

    def test_output_name_order_is_exact(self):
        result = self.run_check('echo "WANAMES b"; echo "WAVALS 1"; echo "OK -3.5 1 -2"',
                                wa=False)
        self.assertEqual(result[1], "WA_NAMES_FAIL")

    def test_output_width_is_exact(self):
        result = self.run_check('echo "WANAMES a"; echo "WAVALS 1 2"; echo "OK -3.5 1 -2"',
                                wa=False)
        self.assertEqual(result[1], "WA_SHAPE_FAIL")

    def test_input_hash_drift_fails(self):
        self.assertEqual(self.run_check('echo "OK -3.5 1 -2"',
                         {**self.REF, "source_sha256": "wrong"})[1], "INPUT_HASH_FAIL")


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


class StrictRecorderTest(unittest.TestCase):
    def test_valid_oracle_is_independent_of_runtime_output(self):
        entry, result, _ = None, None, None
        ref = ["OK", "-3.5", "1", "-2"]
        output = "WANAMES a\nWAVALS 1\nOK -3.5 1 -2\n"
        with unittest.mock.patch("verify_sample.evaluate", return_value=(
                ref, output, ["EVAL_FAIL"], "EVAL_FAIL broken")):
            entry, result, _ = verify_sample.record_model(
                "fixture", pathlib.Path("fixture.stan"), pathlib.Path("fixture.json"),
                pathlib.Path("ref"), pathlib.Path("check"), strict=True)
        self.assertEqual(result["status"], "CMDSTAN_ONLY")
        self.assertEqual(len(entry["points"]), 3)
        for pt in entry["points"].values():
            self.assertEqual(pt["values"], ref[1:])
            self.assertEqual(pt["wa"], {"names": "a", "values": ["1"]})
            self.assertEqual(pt["status"], "CMDSTAN_ONLY")

    def test_missing_nonfinite_ragged_or_duplicate_outputs_fail(self):
        cases = ["OK -3.5 1 -2", "WANAMES a\nWAVALS nan",
                 "WANAMES a,b\nWAVALS 1", "WANAMES a,a\nWAVALS 1 2"]
        for output in cases:
            with self.subTest(output=output), self.assertRaises(ValueError):
                verify_sample.strict_outputs("fixture", 0, ["OK", "-3.5", "1", "-2"], output)

    def test_refusal_or_nonfinite_gradient_is_not_a_strict_reference(self):
        for fields in (["EVAL_FAIL"], ["OK", "-3.5", "nan"]):
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                verify_sample.strict_outputs("fixture", 0, fields, "WANAMES a\nWAVALS 1")


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

    def test_every_local_model_carries_a_reference(self):
        # The replay iterates the reference file, so a model that failed to
        # record is not a failure anywhere: it is simply never run.
        models = load_refs()[0]
        self.assertFalse(set(local_cases()) - set(models))

    def test_imported_references_retain_identity_outputs_and_original_rig(self):
        models, _ = load_refs()
        imported = [case for case in local_cases().values()
                    if case.metadata.get("sampling_smoke")]
        self.assertTrue(imported)
        for case in imported:
            ref = models[case.name]
            self.assertTrue(ref["strict"])
            self.assertEqual(ref["source_sha256"], source_digest(case.source))
            self.assertEqual(ref["data_sha256"], source_digest(case.data))
            self.assertTrue(ref["parameter_names"])
            self.assertIn("compiler", ref["recorded"])
            self.assertIn("reference_flags", ref["recorded"])
            for pt in ref["points"].values():
                names = pt["wa"]["names"].split(",")
                self.assertEqual(len(names), len(pt["wa"]["values"]))
                self.assertFalse(set(ref["parameter_names"]) - set(names))

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


class CorpusArchiveTest(unittest.TestCase):
    """Every local corpus archive is the corpus schema, at the pins."""

    def test_every_archive_loads_with_hashes_at_the_pins(self):
        for name, corpus in verify_refs.CORPORA.items():
            refs, _ = verify_refs.load_corpus_refs(corpus)
            required = () if corpus.unpinned else ("cmdstan", "stan", "math")
            with tempfile.TemporaryDirectory() as tmp:
                models = (corpus.models(pathlib.Path(tmp)) if corpus.models
                          else refs)
            for model in models:
                ref = refs[model]
                with self.subTest(corpus=name, model=model):
                    self.assertEqual(
                        verify_refs.pin_drift(ref["recorded"], required), "")
                    self.assertIn("source_sha256", ref)
                    self.assertIn("data_sha256", ref)

    def test_a_pin_bump_requires_a_new_recording(self):
        recorded = {key: toolchain.pins()[pin] for key, pin in verify_refs.PINS}
        self.assertEqual(verify_refs.pin_drift(recorded), "")
        bumped = {**toolchain.pins(), "MATH_SHA": "0" * 40}
        with unittest.mock.patch.object(toolchain, "pins", lambda: bumped):
            self.assertIn("re-record", verify_refs.pin_drift(recorded))
        self.assertIn("without its math",
                      verify_refs.pin_drift({"cmdstan": recorded["cmdstan"],
                                             "stan": recorded["stan"]}))


class PlatformReferenceTest(unittest.TestCase):
    """Platform-specific answers must never silently reduce oracle coverage."""

    def setUp(self):
        self.primary = {"platform": "Darwin arm64", "cmdstan": "c",
                        "stan": "s", "math": "m", "stanc3": "f"}
        self.rig = {**self.primary, "platform": "Linux x86_64"}
        self.entry = {**REF, "source_sha256": "source", "data_sha256": "data",
                      "points": {str(p): {**VALUES, "wa": {"names": "a", "values": ["1"]}}
                                 for p in POINTS}}

    def select(self, supplement=None, rig=None, target="Linux x86_64",
               compiler=None, compiler_refs=None, compiler_rig=None):
        supplement = {MODEL: self.entry} if supplement is None else supplement
        recordings = [({MODEL: REF}, self.primary), (supplement, rig or self.rig)]
        if compiler == "gcc":
            recordings.append((compiler_refs if compiler_refs is not None else {MODEL: self.entry},
                               compiler_rig or {**self.rig, "compiler_family": "gcc"}))
        with unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}, clear=True), \
             unittest.mock.patch("verify_refs.load_refs", side_effect=recordings):
            return verify_refs.replay_refs(target, compiler)[0]

    def test_matching_platform_selects_its_independent_values_and_provenance(self):
        selected = self.select()[MODEL]
        self.assertEqual(selected["points"], self.entry["points"])
        self.assertEqual(selected["recorded"], self.rig)
        self.assertNotIn("recorded", REF)

    def test_unrecorded_target_retains_primary_reference(self):
        selected = self.select(target="WebAssembly wasm32")[MODEL]
        self.assertEqual(selected["points"], REF["points"])
        self.assertEqual(selected["recorded"], self.primary)
        self.assertIsNone(verify_refs.ulp_limit_for(MODEL, selected, "WebAssembly wasm32"))

    def test_missing_fixture_fails_instead_of_falling_back(self):
        with self.assertRaisesRegex(SystemExit, "cover every"):
            self.select(supplement={})

    def test_compiler_selects_one_reference_without_accepting_the_other_answer(self):
        entry = copy.deepcopy(self.entry)
        entry["source_sha256"] = source_digest(REPO / "tests/regressions" / f"{MODEL}.stan")
        entry["data_sha256"] = source_digest(REPO / "tests/regressions" / f"{MODEL}.json")
        rounded = 1.0 + 244 * math.ulp(1.0)
        for pt in entry["points"].values():
            pt["values"] = ["-3.5", str(rounded), "-2"]
        selected = self.select(compiler="gcc", compiler_refs={MODEL: entry})[MODEL]
        self.assertEqual(selected["recorded"]["compiler_family"], "gcc")
        with tempfile.TemporaryDirectory() as tmp, \
             unittest.mock.patch.dict(verify_refs.ULP_LIMITS, {MODEL: 10}):
            for value, verdict in ((rounded, "OK"), (1.0, "ULP_GATE")):
                result = check_model(MODEL, selected, REPO / "nonexistent-pdb",
                                     stub(tmp, f'echo "OK -3.5 {value!r} -2"'),
                                     pathlib.Path(tmp), 60, 1e-9,
                                     target_platform="Linux x86_64")
                self.assertEqual(result[1], verdict)

    def test_empty_or_wrong_compiler_recording_fails(self):
        with self.assertRaises(SystemExit):
            self.select(compiler="gcc", compiler_refs={})
        with self.assertRaisesRegex(SystemExit, "compiler mismatch"):
            self.select(compiler="gcc", compiler_rig={**self.rig, "compiler_family": "clang"})

    def test_runtime_compiler_comes_from_the_tested_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            for compiler in ("clang", "gcc"):
                checker = stub(tmp, f'echo {compiler}', wa=False)
                self.assertEqual(verify_refs.runtime_compiler(checker), compiler)
            for body in ('echo clang; exit 1', 'echo unrecognized'):
                with self.assertRaisesRegex(SystemExit, "cannot identify"):
                    verify_refs.runtime_compiler(stub(tmp, body, wa=False))

    def test_changed_pins_or_wrong_platform_fail(self):
        for key in ("platform", "cmdstan", "stan", "math", "stanc3"):
            with self.subTest(key=key), self.assertRaisesRegex(SystemExit, "mismatch"):
                self.select(rig={**self.rig, key: "different"})

    def test_incomplete_recordings_fail(self):
        for key in ("source_sha256", "data_sha256", "points"):
            entry = copy.deepcopy(self.entry)
            del entry[key]
            with self.subTest(key=key), self.assertRaises(SystemExit):
                self.select(supplement={MODEL: entry})
        for key in ("wa", "values"):
            entry = copy.deepcopy(self.entry)
            del entry["points"]["1"][key]
            with self.subTest(key=key), self.assertRaises(SystemExit):
                self.select(supplement={MODEL: entry})


class SparsePlatformReferenceTest(unittest.TestCase):
    """A platform correction must preserve the existing measured contract."""

    def setUp(self):
        self.primary = {"platform": "Darwin arm64", "cmdstan": "c",
                        "stan": "s", "math": "m", "stanc3": "f"}
        self.rig = {**self.primary, "platform": "Darwin x86_64",
                    "compiler_family": "clang"}
        self.original = copy.deepcopy(REF)
        for pt in self.original["points"].values():
            pt.update(status="MISMATCH", max_rel=0.006)
        self.entry = copy.deepcopy(self.original)
        self.entry.update(
            source_sha256=source_digest(REPO / "tests/regressions" / f"{MODEL}.stan"),
            data_sha256=source_digest(REPO / "tests/regressions" / f"{MODEL}.json"))
        for pt in self.entry["points"].values():
            pt["values"] = ["-3.5", "1.01", "-2"]

    def select(self, entry=None, rig=None, compiler="clang", models=None):
        extra = {MODEL: self.entry if entry is None else entry}
        with unittest.mock.patch.dict(verify_refs.SPARSE_PLATFORM_REFS, {
                "Darwin x86_64": (pathlib.Path("sparse.json.gz"), "clang",
                                  frozenset({MODEL} if models is None else models))},
                clear=True), unittest.mock.patch("verify_refs.load_refs", side_effect=[
                    ({MODEL: self.original}, self.primary), (extra, rig or self.rig)]):
            return verify_refs.replay_refs("Darwin x86_64", compiler)[0][MODEL]

    def test_selects_independent_values_preserving_gates_and_absent_wa(self):
        selected = self.select()
        self.assertEqual(selected["recorded"], self.rig)
        for point in POINTS:
            before = self.original["points"][str(point)]
            after = selected["points"][str(point)]
            self.assertEqual(after["values"], ["-3.5", "1.01", "-2"])
            self.assertNotIn("wa", after)
            with unittest.mock.patch.dict(verify_refs.MISMATCH_OUTPUTS, {
                    MODEL: verify_refs.Mismatch((1,), "a test")}):
                self.assertEqual(verify_refs.gate_for(MODEL, before, 1e-9),
                                 verify_refs.gate_for(MODEL, after, 1e-9))
        self.assertIsNone(verify_refs.ulp_limit_for(MODEL, selected, "Darwin x86_64"))

    def test_platform_compiler_and_dependency_provenance_fail_closed(self):
        for key in ("platform", "compiler_family", "cmdstan", "stan", "math", "stanc3"):
            with self.subTest(key=key), self.assertRaisesRegex(SystemExit, "mismatch"):
                self.select(rig={**self.rig, key: "wrong"})
        for compiler in (None, "gcc", "unknown"):
            with self.subTest(compiler=compiler), self.assertRaisesRegex(SystemExit, "compiler"):
                self.select(compiler=compiler)

    def test_declared_fixture_set_is_required(self):
        with self.assertRaisesRegex(SystemExit, "declared fixtures"):
            self.select(models={MODEL, "missing"})

    def test_missing_hashes_or_points_fail_closed(self):
        for key in ("source_sha256", "data_sha256"):
            entry = copy.deepcopy(self.entry)
            del entry[key]
            with self.subTest(key=key), self.assertRaisesRegex(SystemExit, "hashes"):
                self.select(entry=entry)
        entry = copy.deepcopy(self.entry)
        del entry["points"]["2"]
        with self.assertRaisesRegex(SystemExit, "points are incomplete"):
            self.select(entry=entry)

    def test_changed_gate_or_coverage_is_refused(self):
        for key, value in (("max_rel", 0.1), ("status", "VERIFIED"),
                           ("wa", {"names": "a", "values": ["1"]}),
                           ("values", ["-3.5", "1"])):
            entry = copy.deepcopy(self.entry)
            entry["points"]["0"][key] = value
            with self.subTest(key=key), self.assertRaises(SystemExit):
                self.select(entry=entry)
        entry = copy.deepcopy(self.entry)
        del entry["points"]["0"]["values"]
        with self.assertRaisesRegex(SystemExit, "coverage changed"):
            self.select(entry=entry)

    def test_selected_answers_still_enforce_numerics_and_actual_input_hashes(self):
        selected = self.select()
        with tempfile.TemporaryDirectory() as tmp, unittest.mock.patch.dict(
                verify_refs.MISMATCH_OUTPUTS, {MODEL: verify_refs.Mismatch((1,), "a test")}):
            for value, verdict in (("1.01", "OK"), ("99", "GATE")):
                result = check_model(MODEL, selected, REPO / "nonexistent-pdb",
                                     stub(tmp, f'echo "OK -3.5 {value} -2"'),
                                     pathlib.Path(tmp), 60, 1e-9,
                                     target_platform="Darwin x86_64")
                self.assertEqual(result[1], verdict)
            for key in ("source_sha256", "data_sha256"):
                changed = {**selected, key: "wrong"}
                result = check_model(MODEL, changed, REPO / "nonexistent-pdb",
                                     stub(tmp, 'echo "OK -3.5 1.01 -2"'),
                                     pathlib.Path(tmp), 60, 1e-9,
                                     target_platform="Darwin x86_64")
                self.assertEqual(result[1], "INPUT_HASH_FAIL")


class WriteRefsTest(unittest.TestCase):
    """Merging one model's values into the committed file."""

    def setUp(self):
        import gzip
        import json
        self.tmp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.tmp.name) / "refs.json.gz"
        self.old = {"cmdstan": "a", "stanc3": "old"}
        self.new = {"cmdstan": "a", "stanc3": "new"}
        self.path.write_bytes(gzip.compress(json.dumps(
            {"schema": SCHEMA, "recorded": self.old,
             "models": {"m1": REF, "m2": REF}}).encode()))
        patcher = unittest.mock.patch.object(
            verify_sample, "REFS_PATH", self.path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.tmp.cleanup)

    def models(self):
        import gzip
        import json
        return json.loads(gzip.decompress(self.path.read_bytes()))["models"]

    def test_merging_keeps_per_model_recording_provenance(self):
        imported = {**REF, "recorded": {"compiler": "another compiler"}}
        write_refs({"imported": imported}, self.old)
        write_refs({"m1": REF}, self.old)
        self.assertEqual(self.models()["imported"]["recorded"], imported["recorded"])
        self.assertEqual(self.models()["m1"]["recorded"], self.old)

    def test_alternate_destination_preserves_primary_recording(self):
        path = self.path.with_name("other-platform.json.gz")
        original = self.path.read_bytes()
        write_refs({"m1": REF}, self.new, path=path)
        models, recorded = load_refs(path)
        self.assertEqual(recorded, self.new)
        self.assertEqual(set(models), {"m1"})
        self.assertEqual(self.path.read_bytes(), original)

    def test_a_partial_run_under_drift_is_refused(self):
        with self.assertRaises(SystemExit):
            write_refs({"m1": REF}, self.new)

    def test_a_full_rerecord_under_drift_starts_a_fresh_file(self):
        write_refs({"m1": REF}, self.new, fresh=True)
        self.assertEqual(sorted(self.models()), ["m1"])
        write_refs({"m2": REF}, self.new, fresh=True)
        self.assertEqual(sorted(self.models()), ["m1", "m2"])


if __name__ == "__main__":
    unittest.main()
