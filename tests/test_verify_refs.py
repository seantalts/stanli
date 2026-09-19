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
import stat
import sys
import tempfile
import unittest
import unittest.mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from verify_refs import (ILL_CONDITIONED, KNOWN_GAPS,  # noqa: E402
                         LOCAL_CORPORA, POINTS, QUARANTINED, SCHEMA,
                         check_model, load_refs, model_files, parse_status,
                         probe_point)
from cmdstan_ref import _result_line  # noqa: E402
import verify_sample  # noqa: E402
import verify_refs  # noqa: E402
from corpus_inventory import local_cases, source_digest
from verify_sample import evaluate, record_wa, write_refs  # noqa: E402

# A model that exists under tests/stanc3, so model_files resolves real
# paths without unpacking a posteriordb dataset. The stub never reads
# them; they only have to be there.
MODEL = "reductions_allowed"
# One point's reference: lp and two gradients, agreeing with CmdStan.
VALUES = {"values": ["-3.5", "1", "-2"], "status": "VERIFIED",
          "max_rel": 0.0, "max_ulp": 0}
REF = {"primary": 0, "points": {str(p): VALUES for p in POINTS}}


def stub(tmp, body, wa=True):
    """A stanli_check that runs `body` with $point set to --point's value.

    A real one answers --wa-values with a write_array row and the replay
    demands one, so the stub emits a row too. Cases about the row itself
    pass wa=False and write their own.
    """
    path = pathlib.Path(tmp) / "stanli_check_stub.sh"
    row = ('if [ -n "$wa" ]; then echo "WANAMES a"; echo "WAVALS 1"; fi\n'
           if wa else "")
    path.write_text("#!/bin/sh\npoint=0\nwa=\n"
                    'while [ $# -gt 0 ]; do\n'
                    '  [ "$1" = --point ] && point=$2\n'
                    '  [ "$1" = --wa-values ] && wa=1\n'
                    '  shift\n'
                    "done\n" + row + body + "\n")
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

    def run_check(self, body, ref=None, model=MODEL, wa=True):
        with tempfile.TemporaryDirectory() as tmp:
            return check_model(model, ref or self.REF,
                               REPO / "nonexistent-pdb", stub(tmp, body, wa),
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

    def test_an_ill_conditioned_model_is_gated_at_the_mismatch_floor(self):
        # Which of these models' points come out clean is a property of
        # the machine that recorded them, so a clean point in one of them
        # gets the floor too, and the run says the floor carried it.
        body = ('if [ "$point" = 2 ]; then echo "OK -3.5 1 -2.00001"; '
                'else echo "OK -3.5 1 -2"; fi')
        self.assertEqual(self.run_check(body)[1], "GATE")
        with unittest.mock.patch.dict(ILL_CONDITIONED, {MODEL: "test"}):
            _, status, _, _, _, _, notes = self.run_check(body)
        self.assertEqual(status, "OK")
        self.assertIn(f"ILL-CONDITIONED {MODEL} point 2: test", " ".join(notes))

    def test_a_known_gap_may_refuse_the_model(self):
        with unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: "test gap"}):
            _, status, _, _, _, _, notes = self.run_check(
                'echo "COMPILE_FAIL unsupported function"')
        self.assertEqual(status, "OK")
        self.assertIn(f"KNOWN GAP {MODEL}: test gap", notes)

    def test_a_known_gap_that_passes_is_reported(self):
        with unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: "test gap"}):
            _, status, _, _, _, detail, _ = self.run_check(
                'echo "OK -3.5 1 -2"')
        self.assertEqual(status, "GAP_CLOSED")
        self.assertIn(MODEL, detail)

    def test_a_known_gap_still_may_not_crash(self):
        with unittest.mock.patch.dict(KNOWN_GAPS, {MODEL: "test gap"}):
            _, status, _, _, _, detail, _ = self.run_check('kill -SEGV $$')
        self.assertEqual(status, "CRASH")
        self.assertIn("signal 11", detail)


class LocalCorpusTest(unittest.TestCase):
    """Models carried in the tree, resolved by name before posteriordb."""

    def test_every_local_model_is_found_where_it_lives(self):
        for directory in LOCAL_CORPORA:
            for stan in sorted(directory.glob("*.stan")):
                found, data = model_files(stan.stem, {}, REPO / "nonexistent",
                                          REPO / "nonexistent")
                self.assertEqual(found, stan)
                self.assertEqual(data, stan.with_suffix(".json"))


class SharedHeaderWorkTest(unittest.TestCase):
    def test_equal_source_basenames_use_separate_model_build_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with unittest.mock.patch("verify_refs.tempfile.mkdtemp", return_value=tmp), \
                 unittest.mock.patch("verify_refs.corpus_models", return_value=[("a", None), ("b", None)]), \
                 unittest.mock.patch("verify_refs.corpus_input", return_value=(root / "model.stan", root / "data.json")), \
                 unittest.mock.patch("verify_refs.subprocess.run", return_value=unittest.mock.Mock(returncode=0, stdout="theta", stderr="")), \
                 unittest.mock.patch("verify_refs.cmdstan_header", return_value=(["theta"], "")) as header:
                self.assertEqual(verify_refs.check_wa_headers(root, root / "check", root / "cmdstan", []), 0)
            self.assertEqual([call.args[1] for call in header.call_args_list], [root / "a", root / "b"])


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
