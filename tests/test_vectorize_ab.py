#!/usr/bin/env python3
"""Unit tests for the measurement report parsers and artifact schema."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))
import vectorize_ab  # noqa: E402


class VectorizeAbTest(unittest.TestCase):
    def test_git_identity_scopes_the_checkout_as_safe(self):
        expected = str(vectorize_ab.REPO.resolve())
        with mock.patch.object(
                vectorize_ab.subprocess, "check_output",
                return_value="0123456789abcdef\n") as check_output:
            self.assertEqual(
                vectorize_ab.git_revision(vectorize_ab.REPO),
                "0123456789abcdef")
        command = check_output.call_args.args[0]
        self.assertIn(f"safe.directory={expected}", command)

        clean = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with mock.patch.object(vectorize_ab.subprocess, "run",
                               return_value=clean) as run:
            self.assertFalse(vectorize_ab.git_tracked_dirty(
                vectorize_ab.REPO))
        command = run.call_args.args[0]
        self.assertIn(f"safe.directory={expected}", command)

    def test_vector_comparison_is_nonfinite_safe(self):
        same = vectorize_ab.compare_vectors(
            ["1", "nan", "-inf"], ["1.0", "nan", "-inf"])
        self.assertTrue(same["same_shape"])
        self.assertEqual(same["changed"], 0)
        self.assertEqual(same["max_rel"], 0.0)
        self.assertTrue(same["nonfinite_match"])

        mismatch = vectorize_ab.compare_vectors(["1"], ["inf"])
        self.assertEqual(mismatch["max_rel"], "inf")
        self.assertEqual(mismatch["changed"], 1)
        self.assertEqual(mismatch["finite_changed"], 0)
        self.assertFalse(mismatch["nonfinite_match"])
        self.assertEqual(mismatch["differences"][0]["index"], 0)
        self.assertRegex(mismatch["differences"][0]["left_bits"],
                         r"^0x[0-9a-f]{16}$")

    def test_execution_status_includes_exit_and_timeout(self):
        ok = {"returncode": 0, "timeout": False}
        rejected = {"returncode": 1, "timeout": False}
        timed_out = {"returncode": 0, "timeout": True}
        self.assertTrue(vectorize_ab.execution_matches_status(ok, ["OK"]))
        self.assertTrue(vectorize_ab.execution_matches_status(
            rejected, ["EVAL_FAIL", "domain"]))
        self.assertFalse(vectorize_ab.execution_matches_status(
            rejected, ["OK", "1"]))
        self.assertFalse(vectorize_ab.execution_matches_status(
            timed_out, ["OK", "1"]))

    def test_ab_only_matching_evaluation_failure_passes(self):
        rejected = {
            "returncode": 1,
            "timeout": False,
            "elapsed_ns": 1,
            "stdout": "EVAL_FAIL identical domain error\n",
            "stderr": "WA none\n",
        }
        with mock.patch.object(vectorize_ab, "run_command",
                               side_effect=[rejected, rejected]):
            result = vectorize_ab.semantic_point(
                "check", "model", "data", {"off": "a", "on": "b"},
                "eight_schools", 0, None, 1, 1e-9)
        self.assertTrue(result["ok"])
        self.assertFalse(result["off"]["reference"]["referenced"])
        self.assertTrue(result["ab"]["error_match"])

    def test_all_nan_row_agrees_with_recorded_evaluation_failure(self):
        nan_row = ["OK", "nan", "nan", "nan"]
        result = vectorize_ab.reference_comparison(
            "s2_invgaussian", nan_row, {"lp": None}, 1e-9)
        self.assertTrue(result["ok"])
        finite_row = ["OK", "1.5", "2.5"]
        result = vectorize_ab.reference_comparison(
            "s2_invgaussian", finite_row, {"lp": None}, 1e-9)
        self.assertFalse(result["ok"])

    def test_default_selection_skips_known_gaps(self):
        refs = {"eight_schools": {}, "s2_com_poisson": {}}
        self.assertEqual(
            vectorize_ab.default_selection(refs, {"eight_schools": None},
                                           {"s2_com_poisson": "why"}),
            ["eight_schools"])

    def test_ab_only_matching_compile_failure_fails(self):
        failed = {
            "returncode": 1,
            "timeout": False,
            "elapsed_ns": 1,
            "stdout": "COMPILE_FAIL identical lower error\n",
            "stderr": "",
        }
        with mock.patch.object(vectorize_ab, "run_command",
                               side_effect=[failed, failed]):
            result = vectorize_ab.semantic_point(
                "check", "model", "data", {"off": "a", "on": "b"},
                "eight_schools", 0, None, 1, 1e-9)
        self.assertFalse(result["ok"])
        self.assertFalse(result["off"]["reference"]["ok"])

    def test_ab_only_finite_values_must_meet_gate(self):
        off = {
            "returncode": 0, "timeout": False, "elapsed_ns": 1,
            "stdout": "OK 1 2\n", "stderr": "WA none\n",
        }
        on = {
            "returncode": 0, "timeout": False, "elapsed_ns": 1,
            "stdout": "OK 1 3\n", "stderr": "WA none\n",
        }
        with mock.patch.object(vectorize_ab, "run_command",
                               side_effect=[off, on]):
            result = vectorize_ab.semantic_point(
                "check", "model", "data", {"off": "a", "on": "b"},
                "eight_schools", 0, None, 1, 1e-9)
        self.assertFalse(result["ok"])
        self.assertTrue(result["ab"]["ab_only_finite_gate"]["applied"])
        self.assertFalse(result["ab"]["ab_only_finite_gate"]["values_ok"])

    def test_ab_only_write_array_values_must_meet_gate(self):
        off = {
            "returncode": 0, "timeout": False, "elapsed_ns": 1,
            "stdout": "WANAMES x\nWAVALS 1\nOK 0\n", "stderr": "",
        }
        on = {
            "returncode": 0, "timeout": False, "elapsed_ns": 1,
            "stdout": "WANAMES x\nWAVALS 2\nOK 0\n", "stderr": "",
        }
        with mock.patch.object(vectorize_ab, "run_command",
                               side_effect=[off, on]):
            result = vectorize_ab.semantic_point(
                "check", "model", "data", {"off": "a", "on": "b"},
                "eight_schools", 0, None, 1, 1e-9)
        self.assertFalse(result["ok"])
        self.assertFalse(
            result["ab"]["ab_only_finite_gate"]["wa_values_ok"])

    def test_write_array_failure_outcomes_do_not_collapse(self):
        absent = vectorize_ab.parse_wa_outcome("OK 1\n")
        failed_a = vectorize_ab.parse_wa_outcome(
            "WANAMES FAIL domain A\nWAVALS FAIL\nOK 1\n")
        failed_b = vectorize_ab.parse_wa_outcome(
            "WANAMES FAIL domain B\nWAVALS FAIL\nOK 1\n")
        self.assertFalse(vectorize_ab.compare_wa_outcomes(
            absent, failed_a)["category_match"])
        compared = vectorize_ab.compare_wa_outcomes(failed_a, failed_b)
        self.assertTrue(compared["category_match"])
        self.assertFalse(compared["reason_match"])

    def test_timeout_output_is_decoded(self):
        timeout = subprocess.TimeoutExpired(
            ["probe"], 1, output=b"OK 1\n", stderr=b"last line\n")
        with mock.patch.object(vectorize_ab.subprocess, "run",
                               side_effect=timeout):
            result = vectorize_ab._run_command_fallback(["probe"], 1)
        self.assertTrue(result["timeout"])
        self.assertEqual(result["stdout"], "OK 1\n")
        self.assertEqual(result["stderr"], "last line\n")
        self.assertIsNone(result["maxrss_bytes"])

    def test_fallback_command_reports_no_maxrss(self):
        completed = subprocess.CompletedProcess(
            [], 0, stdout="out\n", stderr="")
        with mock.patch.object(vectorize_ab.subprocess, "run",
                               return_value=completed):
            result = vectorize_ab._run_command_fallback(["probe"], 1)
        self.assertFalse(result["timeout"])
        self.assertEqual(result["returncode"], 0)
        self.assertIsNone(result["maxrss_bytes"])

    def test_run_command_captures_output_and_returncode(self):
        result = vectorize_ab.run_command(
            [sys.executable, "-c",
             "import sys; print('out'); print('err', file=sys.stderr); "
             "sys.exit(3)"], 5)
        self.assertFalse(result["timeout"])
        self.assertEqual(result["returncode"], 3)
        self.assertEqual(result["stdout"].strip(), "out")
        self.assertEqual(result["stderr"].strip(), "err")
        self.assertIn("maxrss_bytes", result)

    def test_run_command_reports_maxrss_on_this_platform(self):
        result = vectorize_ab.run_command(
            [sys.executable, "-c", "pass"], 5)
        if hasattr(os, "wait4"):
            self.assertIsInstance(result["maxrss_bytes"], int)
            self.assertGreater(result["maxrss_bytes"], 0)
        else:
            self.assertIsNone(result["maxrss_bytes"])

    def test_run_command_times_out_and_kills_the_child(self):
        result = vectorize_ab.run_command(
            [sys.executable, "-c", "import time; time.sleep(2)"], 0.05)
        self.assertTrue(result["timeout"])
        self.assertIsNone(result["returncode"])

    def test_candidate_pass_selects_only_one_source_pass(self):
        completed = {
            "returncode": 0,
            "timeout": False,
            "elapsed_ns": 1,
            "stdout": "",
            "stderr": "",
        }
        output = pathlib.Path("candidate.mir")
        with mock.patch.object(vectorize_ab, "run_command",
                               return_value=completed) as run:
            vectorize_ab.compile_source(
                "probe", pathlib.Path("model.stan"), output,
                vectorize_ab.VECTORIZE_LOOPS, False, 1)
        self.assertEqual(run.call_args.args[0], [
            "probe", "--vectorize-loops", "off",
            "--output", output, pathlib.Path("model.stan"),
        ])

    def test_prep_dispositions_are_parsed(self):
        rows = vectorize_ab.parse_prep(
            "noise\n"
            "stanli_prep graph=log_prob stage=reroll ns=41 ops=7 "
            "regions=2 packed_rows=1 term_density=3 element_density=4 "
            "term_widen=5 element_store=6\n")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["graph"], "log_prob")
        self.assertEqual(rows[0]["packed_rows"], 1)
        self.assertEqual(rows[0]["element_store"], 6)

    def test_reroll_summary_keeps_both_final_op_counts(self):
        rows = vectorize_ab.parse_prep(
            "stanli_prep graph=log_prob stage=total ns=1 ops=7 slots=8\n"
            "stanli_prep graph=write_array stage=total ns=2 ops=5 slots=6\n"
            "stanli_prep graph=log_prob stage=reroll ns=3 regions=1 "
            "packed_rows=0 term_density=1 element_density=0 term_widen=0 "
            "element_store=0\n"
            "stanli_prep graph=write_array stage=reroll ns=4 regions=2 "
            "packed_rows=0 term_density=0 element_density=0 term_widen=0 "
            "element_store=2\n")
        evidence = vectorize_ab.summarize_reroll([{
            "source_pass": "on",
            "runtime_reroll": "on",
            "samples": [{"sample": 1, "rows": rows}],
        }])
        sample = evidence[0]["samples"][0]
        self.assertEqual(sample["log_prob"]["final_ops"], 7)
        self.assertEqual(sample["log_prob"]["final_slots"], 8)
        self.assertEqual(sample["write_array"]["final_ops"], 5)
        self.assertEqual(sample["write_array"]["final_slots"], 6)
        self.assertEqual(sample["write_array"]["element_store"], 2)

    def test_dump_summary_is_parsed(self):
        parsed = vectorize_ab.parse_dump(
            "slots=19 ops=7 result=18\n"
            "SUMMARY ops=7 scalar_out=2 vector_out=5\n"
            "  NORMAL_LPDF total=1 scalar=0\n")
        self.assertEqual(parsed["ops"], 7)
        self.assertEqual(parsed["summary"]["scalar_out"], 2)
        self.assertEqual(parsed["opcodes"]["NORMAL_LPDF"]["total"], 1)
        self.assertEqual(vectorize_ab.dump_problems(parsed), [])
        self.assertIn("missing SUMMARY",
                      vectorize_ab.dump_problems({"slots": 1, "ops": 1,
                                                  "result": 0}))

    def test_gradient_output_calibration_and_abba_order(self):
        self.assertIsNone(vectorize_ab.parse_bench_output(
            "1 2 3 4\nnot the final row\n"))
        parsed = vectorize_ab.parse_bench_output(
            "model output\n200.0 3.5 80.0 7\n")
        self.assertEqual(parsed["gradient_ns"], 200.0)
        self.assertEqual(parsed["n_params"], 7)
        self.assertEqual(vectorize_ab.calibrated_iterations(
            [100.0, 200.0], 0.001, 10_000), 5000)

        calls = []

        def fake_run(_bench, mir, _data, iterations, _timeout):
            mode = str(mir)
            calls.append((mode, iterations))
            latency = 100.0 if mode == "off" else 200.0
            return {
                "ok": True,
                "returncode": 0,
                "timeout": False,
                "elapsed_ns": 1,
                "stderr_tail": "",
                "result": {
                    "gradient_ns": latency,
                    "sink": 0.0,
                    "forward_ns": 50.0,
                    "n_params": 2,
                },
            }

        with mock.patch.object(vectorize_ab, "bench_run",
                               side_effect=fake_run):
            measured = vectorize_ab.gradient_benchmark(
                "bench", {"off": "off", "on": "on"}, "data", 2, 8,
                0.001, 10_000, 5)
        self.assertEqual(calls[:2], [("off", 8), ("on", 8)])
        self.assertEqual(
            [run["source_pass"] for run in measured["runs"]],
            ["off", "on", "on", "off"] * 2)
        self.assertEqual(measured["iterations"], 5000)
        self.assertEqual(measured["on_over_off"], 2.0)

    def test_gradient_ratio_exceeds_threshold(self):
        threshold = vectorize_ab.GRADIENT_RATIO_THRESHOLD
        self.assertFalse(vectorize_ab.gradient_ratio_exceeds(
            {"ok": True, "on_over_off": threshold}))
        self.assertFalse(vectorize_ab.gradient_ratio_exceeds(
            {"ok": True, "on_over_off": threshold - 0.001}))
        self.assertTrue(vectorize_ab.gradient_ratio_exceeds(
            {"ok": True, "on_over_off": threshold + 0.001}))
        self.assertFalse(vectorize_ab.gradient_ratio_exceeds(
            {"ok": True, "on_over_off": None}))
        self.assertFalse(vectorize_ab.gradient_ratio_exceeds(None))

    def test_gradient_gate_requires_a_confirming_rerun(self):
        threshold = vectorize_ab.GRADIENT_RATIO_THRESHOLD
        grown = {"ok": True, "on_over_off": threshold + 0.05}
        calm = {"ok": True, "on_over_off": 1.0}
        confirmed = {"ok": True, "on_over_off": threshold + 0.03}
        unavailable = {"ok": False, "on_over_off": threshold + 0.05}

        # No initial excursion: never asks for a re-run and never fails.
        self.assertEqual(vectorize_ab.gradient_gate("m", calm, None), [])

        # Initial excursion but no re-run recorded yet: report only.
        self.assertEqual(vectorize_ab.gradient_gate("m", grown, None), [])

        # Re-run comes back calm: the excursion was noise, no failure.
        self.assertEqual(vectorize_ab.gradient_gate("m", grown, calm), [])

        # A re-run that could not be measured is not treated as confirming.
        self.assertEqual(
            vectorize_ab.gradient_gate("m", grown, unavailable), [])

        # Both the initial measurement and the fresh re-run exceed: fail.
        failures = vectorize_ab.gradient_gate("m", grown, confirmed)
        self.assertEqual(len(failures), 1)
        self.assertIn("m", failures[0])
        self.assertIn(f"{grown['on_over_off']:.4f}", failures[0])
        self.assertIn(f"{confirmed['on_over_off']:.4f}", failures[0])

    def _write_graphs_jsonl(self, directory, records):
        with (pathlib.Path(directory) / "graphs.jsonl").open("w") as stream:
            for record in records:
                stream.write(json.dumps(record) + "\n")

    def _cell_record(self, model, source_pass, runtime_reroll, final_ops,
                     lower_ops, regions, final_slots=100):
        return {
            "model": model,
            "source_pass": source_pass,
            "runtime_reroll": runtime_reroll,
            "graph": {"ops": final_ops, "slots": final_slots},
            "samples": [{"sample": 1, "rows": [
                {"graph": "log_prob", "stage": "lower", "ops": lower_ops},
                {"graph": "log_prob", "stage": "island", "regions": regions,
                 "ns": 10},
            ]}],
        }

    def test_load_cells_returns_none_without_graphs_jsonl(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertIsNone(vectorize_ab.load_cells(empty))

    def test_baseline_diff_flags_a_runtime_wide_regression(self):
        # Both the off and on cells grow identically: invisible to the
        # on/off gate, which is exactly what a baseline comparison is for.
        before = [
            self._cell_record("M0_model", "off", "on", 34, 1013, 0),
            self._cell_record("M0_model", "on", "on", 34, 1013, 0),
            self._cell_record("stable_model", "off", "on", 20, 50, 0),
            self._cell_record("stable_model", "on", "on", 20, 50, 0),
        ]
        after = [
            self._cell_record("M0_model", "off", "on", 62, 1013, 0),
            self._cell_record("M0_model", "on", "on", 62, 1013, 0),
            self._cell_record("stable_model", "off", "on", 20, 50, 0),
            self._cell_record("stable_model", "on", "on", 20, 50, 0),
        ]
        with tempfile.TemporaryDirectory() as before_dir, \
                tempfile.TemporaryDirectory() as after_dir:
            self._write_graphs_jsonl(before_dir, before)
            self._write_graphs_jsonl(after_dir, after)
            before_cells = vectorize_ab.load_cells(before_dir)
            after_cells = vectorize_ab.load_cells(after_dir)
            comparison = vectorize_ab.baseline_comparison_report(
                before_dir, before_cells, after_cells)
        self.assertTrue(comparison["available"])
        self.assertEqual(comparison["cells_compared"], 4)
        self.assertEqual(comparison["changed_cells"], 2)
        self.assertEqual(comparison["final_ops_grew_models"], ["M0_model"])
        self.assertEqual(comparison["final_ops_shrank_models"], [])
        cells = {(c["source_pass"], c["runtime_reroll"])
                for c in comparison["changes"]}
        self.assertEqual(cells, {("off", "on"), ("on", "on")})

    def test_baseline_comparison_reports_missing_baseline(self):
        comparison = vectorize_ab.baseline_comparison_report(
            "/no/such/dir", None, {})
        self.assertFalse(comparison["available"])
        self.assertIn("reason", comparison)

    def test_baseline_comparison_is_first_in_summary_md(self):
        graph = self._cell_record("probe", "off", "on", 5, 5, 0)
        graph["samples"][0]["elapsed_ns"] = 1
        with tempfile.TemporaryDirectory() as temp:
            out = pathlib.Path(temp)
            comparison = {
                "baseline_dir": "/baseline",
                "available": True,
                "cells_compared": 1,
                "changed_cells": 1,
                "final_ops_grew_models": ["probe"],
                "final_ops_shrank_models": [],
                "changes": [{
                    "model": "probe", "source_pass": "off",
                    "runtime_reroll": "on",
                    "before": {"final_ops": 5, "lower_ops": 5,
                              "regions": 0, "final_slots": 100},
                    "after": {"final_ops": 9, "lower_ops": 5,
                             "regions": 0, "final_slots": 100},
                }],
            }
            vectorize_ab.write_reports(
                out, {"schema": 1}, [], [graph], [{
                    "model": "probe", "mir_changed": False,
                    "changed_values": 0, "points": 1,
                }], [], [], [], [], baseline_comparison=comparison)
            text = (out / "summary.md").read_text()
        title_index = text.index("# MIR source-pass A/B measurement")
        baseline_index = text.index("## Baseline comparison")
        outcome_index = text.index("Outcome:")
        self.assertLess(title_index, baseline_index)
        self.assertLess(baseline_index, outcome_index)
        self.assertIn("probe", text[baseline_index:outcome_index])

    def test_op_count_gate_compares_reroll_on_cells(self):
        def cell(source_pass, runtime_reroll, final_ops, lowered_ops):
            return {
                "source_pass": source_pass,
                "runtime_reroll": runtime_reroll,
                "graph": {"ops": final_ops},
                "samples": [{"sample": 1, "rows": [{
                    "graph": "log_prob", "stage": "lower", "ns": 1,
                    "ops": lowered_ops,
                }]}],
            }

        grown = vectorize_ab.op_count_gate("probe", [
            cell("off", "on", 104, 14041), cell("on", "on", 163, 8311)])
        self.assertEqual(len(grown), 1)
        self.assertIn("104 -> 163", grown[0])
        self.assertEqual(vectorize_ab.op_count_gate("probe", [
            cell("off", "on", 16, 91), cell("on", "on", 17, 14)]), [])
        self.assertEqual(vectorize_ab.op_count_gate("probe", [
            cell("off", "on", 10, 10), cell("on", "on", 11, 10)]), [])
        lowered = vectorize_ab.op_count_gate("probe", [
            cell("off", "on", 7, 6), cell("on", "on", 7, 8)])
        self.assertEqual(len(lowered), 1)
        self.assertIn("6 -> 8", lowered[0])
        self.assertEqual(vectorize_ab.op_count_gate("probe", [
            cell("off", "off", 27, 1), cell("on", "off", 10522, 2),
            cell("off", "on", 27, 1)]), [])
        missing = cell("on", "on", None, None)
        missing["graph"] = {}
        missing["samples"] = []
        self.assertEqual(vectorize_ab.op_count_gate("probe", [
            cell("off", "on", 27, 58449), missing]), [])
        expected = next(iter(vectorize_ab.LOWERED_GROWTH_EXPECTED))
        self.assertEqual(vectorize_ab.op_count_gate(expected, [
            cell("off", "on", 315, 1348), cell("on", "on", 315, 1628)]), [])
        final_grew = vectorize_ab.op_count_gate(expected, [
            cell("off", "on", 315, 1348), cell("on", "on", 400, 1628)])
        self.assertEqual(len(final_grew), 1)
        self.assertIn("315 -> 400", final_grew[0])

    def test_report_writes_all_artifacts(self):
        graph = {
            "model": "probe",
            "source_pass": "off",
            "runtime_reroll": "on",
            "graph": {"summary": {"ops": 1, "scalar_out": 1}},
            "samples": [{
                "sample": 1,
                "elapsed_ns": 4,
                "maxrss_bytes": 12345,
                "rows": [{
                    "graph": "log_prob", "stage": "reroll", "ns": 3,
                    "regions": 0, "packed_rows": 0, "term_density": 0,
                    "element_density": 0, "term_widen": 0,
                    "element_store": 0,
                }],
            }],
        }
        with tempfile.TemporaryDirectory() as temp:
            out = pathlib.Path(temp)
            summary = vectorize_ab.write_reports(
                out, {"schema": 1}, [{"model": "probe", "ok": True}],
                [graph], [{
                    "model": "probe", "mir_changed": False,
                    "changed_values": 0, "points": 1,
                }], [], [], [], [])
            self.assertTrue(summary["ok"])
            expected = {
                "manifest.json", "corpus.jsonl", "graphs.jsonl",
                "bench.tsv", "summary.json", "summary.md",
            }
            self.assertEqual({path.name for path in out.iterdir()}, expected)
            self.assertTrue(json.loads((out / "summary.json").read_text())["ok"])
            bench_lines = (out / "bench.tsv").read_text().splitlines()
            header = bench_lines[0]
            self.assertIn("write_array_ops", header)
            self.assertIn("log_prob_slots", header)
            self.assertIn("write_array_slots", header)
            self.assertIn("prep_maxrss_bytes", header)
            self.assertIn("gradient_maxrss_bytes", header)
            self.assertIn("log_prob_reroll_packed_rows", header)
            self.assertIn("write_array_reroll_element_store", header)
            columns = header.split("\t")
            preparation_row = next(
                row for row in bench_lines[1:]
                if row.split("\t")[columns.index("measurement")]
                == "preparation")
            self.assertEqual(
                preparation_row.split("\t")[
                    columns.index("prep_maxrss_bytes")], "12345")
            summary = vectorize_ab.write_reports(
                out, {"schema": 1}, [], [graph], [{
                    "model": "probe", "mir_changed": True,
                    "changed_values": 0, "points": 1,
                }], [], [], ["probe: final log_prob ops grew 27 -> 10522"],
                [])
            self.assertTrue(summary["ok"])
            self.assertEqual(len(summary["op_count_diagnostics"]), 1)
            self.assertIn("## Op count diagnostics",
                          (out / "summary.md").read_text())

            summary = vectorize_ab.write_reports(
                out, {"schema": 1}, [], [graph], [{
                    "model": "probe", "mir_changed": False,
                    "changed_values": 0, "points": 1,
                }], [], [], [], ["probe: gradient on/off 1.09 exceeds 1.04, "
                                 "confirmed at 1.07"])
            self.assertFalse(summary["ok"])
            self.assertEqual(len(summary["gradient_failures"]), 1)
            self.assertIn("## Gradient time failures",
                          (out / "summary.md").read_text())


if __name__ == "__main__":
    unittest.main()
