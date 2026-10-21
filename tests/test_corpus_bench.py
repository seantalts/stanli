#!/usr/bin/env python3
"""Focused tests for the corpus benchmark TSV serializer."""

import csv
import contextlib
import copy
import json
import os
import tempfile
import zipfile
import io
import pathlib
import sys
import unittest
from unittest import mock
from types import SimpleNamespace

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from harnesses.corpus_bench import (COLS, PROTOCOL, row_line,
    upgrade_header, Runner, PhaseFailure, parse_timing, check_pair,
    summarize_pairs, paired_order, open_run, measure_model,
    benchmark_cases, materialize_data, build_model, run_config)


class EnvironmentTests(unittest.TestCase):
    def test_compiler_experiment_overrides_are_rejected_before_measurement(self):
        from harnesses.corpus_bench import main
        for key in ("STANLI_BOUNDED_SPECIALIZATION", "STANLI_STRUCTURED_LOOPS",
                    "STANLI_ISLAND_ALWAYS", "STANLI_WA_FORCE_INTERP",
                    "STANLI_PACKET_MATH"):
            with self.subTest(key=key), mock.patch.dict(os.environ, {key: "0"}, clear=True):
                error = io.StringIO()
                with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
                    main(["unused-cmdstan", "unused-pdb", "unused.tsv"])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn(key, error.getvalue())


class RowLineTests(unittest.TestCase):
    def parse(self, row):
        text = "\t".join(COLS) + "\n" + row_line(row)
        return next(csv.DictReader(io.StringIO(text), delimiter="\t"))

    def test_empty_final_note_is_quoted_without_changing_its_value(self):
        line = row_line({"model": "m", "note": ""})
        self.assertTrue(line.endswith('\t""\n'))
        self.assertFalse(line.endswith("\t\n"))
        self.assertEqual(self.parse({"model": "m", "note": ""})["note"], "")

    def test_nonempty_note_is_unchanged(self):
        row = {"model": "m", "note": "stanli_sample_timeout"}
        self.assertTrue(row_line(row).endswith("\tstanli_sample_timeout\n"))
        self.assertEqual(self.parse(row)["note"], "stanli_sample_timeout")

    def test_missing_preparation_duration_stays_missing(self):
        old_cols = [c for c in COLS if c != "stanli_prep_s"]
        text = ("\t".join(old_cols) + "\n"
                + "\t".join("m" if c == "model" else "" for c in old_cols)
                + "\n")
        old_row = next(csv.DictReader(io.StringIO(text), delimiter="\t"))
        self.assertEqual(self.parse(old_row)["stanli_prep_s"], "")


class UpgradeHeaderTests(unittest.TestCase):
    def test_noop_when_header_already_current(self):
        self.assertIsNone(upgrade_header(COLS, {}))

    def test_rewrites_old_header_to_current_cols(self):
        old_cols = [c for c in COLS if c != "stanli_prep_s"]
        rows = {"m": {**{c: "" for c in old_cols}, "model": "m"}}
        text = upgrade_header(old_cols, rows)
        header = text.splitlines()[0]
        self.assertEqual(header, "\t".join(COLS))


def driver_output(ns=250000, points=3, dropped=(), iterations=1000, values=None, **top):
    """What the --timed drivers print: one record per evaluation point."""
    per_point = ns if isinstance(ns, (list, tuple)) else [ns] * points
    entries = []
    for k in range(points):
        if k in dropped:
            entries.append(dict(point=k, finite=False))
            continue
        entries.append(dict(point=k, finite=True, iterations=iterations, batch=8,
                            elapsed_ns=int(per_point[k] * iterations),
                            values=values(k) if values else [-3.0 - k, 2.0 + k]))
    return dict(dict(protocol="stanli-gradient-v3", warmup_iterations=5000,
                     warmup_elapsed_ns=20000000, points=entries), **top)


class TimingTests(unittest.TestCase):
    def test_print_output_before_json_is_allowed(self):
        result = driver_output()
        self.assertEqual(parse_timing("Stan print\n" + json.dumps(result), 20, 40, 3), result)

    def test_rejects_short_windows_and_malformed_results(self):
        def point(index, **change):
            def edit(result):
                result["points"][index].update(change)
            return edit
        def drop(index, key):
            def edit(result):
                del result["points"][index][key]
            return edit
        def replace(**change):
            return lambda result: result.update(change)
        edits = {
            "short warmup": replace(warmup_elapsed_ns=1000),
            "no warmup iterations": replace(warmup_iterations=0),
            "zero iterations": point(1, iterations=0),
            "short measurement": point(2, elapsed_ns=1),
            "zero batch": point(0, batch=0),
            "nan value": point(0, values=[float("nan")]),
            "empty values": point(0, values=[]),
            "missing values": drop(0, "values"),
            "old protocol": replace(protocol="stanli-gradient-v2"),
            "unknown protocol": replace(protocol="old"),
            "too few points": lambda result: result["points"].pop(),
            "wrong index": point(1, point=5),
            "finite is not a bool": point(0, finite=1),
            "values on a dropped point": lambda result: result["points"][1].update(
                finite=False, values=[1.0]),
            "no finite point": lambda result: [point(k, finite=False)(result) for k in range(3)],
        }
        for label, edit in edits.items():
            result = driver_output()
            edit(result)
            with self.subTest(label), self.assertRaises(PhaseFailure):
                parse_timing(json.dumps(result), 20, 40, 3)
        with self.assertRaises(PhaseFailure):
            parse_timing("not JSON", 20, 40, 3)

    def test_non_finite_points_are_accepted_without_measurements(self):
        result = driver_output(dropped=(1,))
        self.assertEqual(parse_timing(json.dumps(result), 20, 40, 3), result)

    def pair(self, stanli=None, cmdstan=None):
        return dict(stanli=stanli or driver_output(), cmdstan=cmdstan or driver_output())

    def test_pair_checks_full_gradient_at_every_point(self):
        with self.assertRaisesRegex(PhaseFailure, "point 2.*mismatch"):
            check_pair(self.pair(cmdstan=driver_output(
                values=lambda k: [-3.0 - k, 4.0 + k if k == 2 else 2.0 + k])))
        with self.assertRaisesRegex(PhaseFailure, "point 0.*widths"):
            check_pair(self.pair(cmdstan=driver_output(values=lambda k: [-3.0 - k])))
        checked = check_pair(self.pair())
        self.assertEqual(checked, dict(kept_points=[0, 1, 2], dropped_points=[], width=2,
                                       scaled_errors=[0, 0, 0], max_scaled_error=0))
        off = check_pair(self.pair(cmdstan=driver_output(
            values=lambda k: [-3.0 - k, 2.0 + k + (1e-10 if k == 1 else 0)])))
        self.assertEqual((off["scaled_errors"][0], off["scaled_errors"][2]), (0, 0))
        self.assertAlmostEqual(off["scaled_errors"][1], 1e-10 / 3, delta=1e-13)
        self.assertEqual(off["max_scaled_error"], off["scaled_errors"][1])

    def test_points_non_finite_in_both_engines_are_dropped(self):
        checked = check_pair(self.pair(driver_output(dropped=(0, 2)), driver_output(dropped=(0, 2))))
        self.assertEqual((checked["kept_points"], checked["dropped_points"]), ([1], [0, 2]))

    def test_point_non_finite_in_one_engine_fails_the_model(self):
        with self.assertRaisesRegex(PhaseFailure, "point 1.*stanli"):
            check_pair(self.pair(stanli=driver_output(dropped=(2,)), cmdstan=driver_output(dropped=(1, 2))))
        with self.assertRaisesRegex(PhaseFailure, "point 1.*cmdstan"):
            check_pair(self.pair(cmdstan=driver_output(dropped=(1,))))

    def test_no_kept_point_fails(self):
        every = driver_output(dropped=(0, 1, 2))
        with self.assertRaisesRegex(PhaseFailure, "no point"):
            check_pair(self.pair(every, every))

    def test_balanced_order_and_paired_statistics(self):
        self.assertEqual([paired_order(i)[0] for i in range(6)],
                         ["stanli", "cmdstan"] * 3)
        pairs = [dict(stanli=driver_output(ns=s, iterations=10**8),
                      cmdstan=driver_output(ns=c, iterations=10**8))
                 for s, c in ((1, 2), (2, 2), (10, 30), (4, 4))]
        result = summarize_pairs(pairs)
        self.assertEqual(result["stanli_ns_grad"], 3)
        self.assertEqual(result["stanli_ns_grad_mad"], 1.5)
        self.assertEqual(result["paired_speedup"], 1.5)

    def test_process_time_is_the_mean_over_kept_points(self):
        pairs = [dict(stanli=driver_output(ns=[100, 200, 300, 9999], points=4, dropped=(3,)),
                      cmdstan=driver_output(ns=[400, 400, 400, 1], points=4, dropped=(3,)))]
        result = summarize_pairs(pairs)
        self.assertEqual(result["stanli_ns_grad"], 200)
        self.assertEqual(result["cmdstan_ns_grad"], 400)
        self.assertEqual(result["paired_speedup"], 2)


class RunnerTests(unittest.TestCase):
    def test_short_runs_use_blocking_wait_without_timeout_polling(self):
        with tempfile.TemporaryDirectory() as temp:
            process = mock.Mock(returncode=0)
            process.wait.return_value = 0
            with mock.patch("harnesses.corpus_bench.subprocess.Popen", return_value=process):
                result = Runner(pathlib.Path(temp)).run("short", ["program"], 5)
            process.wait.assert_called_once_with()
            self.assertEqual(result["status"], "ok")

    def test_nonzero_exit_never_counts_as_success_and_logs_survive(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = Runner(pathlib.Path(temp))
            with self.assertRaises(PhaseFailure):
                runner.require("failing", [sys.executable, "-c",
                               "print('looks successful'); raise SystemExit(7)"], 5)
            event = json.loads(runner.events.read_text())
            self.assertEqual(event["returncode"], 7)
            self.assertEqual(event["status"], "failed")
            self.assertIn("looks successful", runner.text(event))

    def test_timeout_is_censored_and_keeps_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = Runner(pathlib.Path(temp))
            result = runner.run("slow", [sys.executable, "-u", "-c",
                                "import time; print('started'); time.sleep(10)"], 0.2)
            self.assertEqual(result["status"], "timeout")
            self.assertEqual(result["timeout_s"], 0.2)
            self.assertIn("started", runner.text(result))


class ManifestTests(unittest.TestCase):
    def test_resume_requires_identical_config_and_input_identities(self):
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp) / "bench.tsv"
            identity = dict(protocol="v2", warmup_ms=200, binary="a", data="b")
            _, original = open_run(output, identity, False)
            self.assertEqual(open_run(output, identity, True)[1], original)
            for change in (dict(warmup_ms=100), dict(binary="new"), dict(data="new"),
                           dict(protocol="stanli-corpus-v4"), dict(gradient_budget=20000)):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    open_run(output, dict(identity, **change), True)

    def test_cannot_append_to_historical_tsv(self):
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp) / "bench.tsv"
            output.write_text("historical data\n")
            with self.assertRaises(ValueError):
                open_run(output, {}, False)
            with self.assertRaises(ValueError):
                open_run(output, {}, True)
            self.assertEqual(output.read_text(), "historical data\n")


class SetupEstimateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source, self.data = self.root / "input.stan", self.root / "input.json"
        self.source.write_text("parameters { real x; } model { x ~ normal(0,1); }")
        self.data.write_text("{}")
        self.args = SimpleNamespace(vectorize_probe=self.root / "probe",
            stanc=self.root / "stanc", stancflags="--O1", build_timeout=900,
            gradient_timeout=60, rounds=2, warmup_ms=20, measure_ms=40, points=3,
            cmdstan=self.root / "cmdstan", bench=self.root / "bench")
        self.events, self.failures = [], {}
        self.preparation = iter(["0.25 s", "0.75 s"])
        self.mismatch = None
        self.dropped = {}
        self.runner = mock.Mock()
        self.runner.run.side_effect = self.run_command
        self.runner.require.side_effect = self.require
        self.runner.text.side_effect = self.text

    def run_command(self, phase, argv, timeout, cwd=None):
        elapsed = {"stanli-mir": 2.0, "stanc-cpp": 3.0, "cmdstan-build": 5.0,
                   "gradient-driver-build": 999.0}.get(phase.split("/")[-1], 1.0)
        event = dict(id=len(self.events), phase=phase, argv=list(map(str, argv)),
                     status=self.failures.get(phase, "ok"), elapsed_s=elapsed)
        self.events.append(event)
        return event

    def require(self, phase, argv, timeout, cwd=None):
        event = self.run_command(phase, argv, timeout, cwd)
        if event["status"] != "ok":
            raise PhaseFailure(f"{phase}: {event['status']} (event {event['id']})")
        return event

    def text(self, event):
        if "/prepare/" in event["phase"]:
            return next(self.preparation)
        _, _, round_index, engine = event["phase"].split("/")
        dropped = self.dropped.get((int(round_index), engine), self.dropped.get(engine, ()))
        bad = self.mismatch if engine == "cmdstan" else None
        return json.dumps(driver_output(
            ns=500000 if engine == "cmdstan" else 250000, dropped=dropped,
            values=lambda k: [-3.0 - k, 20.0 if k == bad else 2.0 + k]))

    def measure(self, reference=None):
        with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]), \
             mock.patch("harnesses.corpus_bench.sha", return_value="test-hash"):
            return measure_model("m", self.source, self.data, self.root,
                                 {"run_id": "test-run"}, self.runner, self.args, reference=reference)

    def test_proxy_includes_each_setup_component_but_not_driver_build(self):
        record = self.measure()
        row = record["row"]
        self.assertEqual(PROTOCOL, "stanli-corpus-v6")
        self.assertEqual(record["status"], "ok")
        self.assertEqual(row["gradient_count"], 5000)
        self.assertEqual(row["stanli_compile_s"], 2)
        self.assertEqual(row["stanli_prep_s"], .5)
        self.assertEqual(row["cmdstan_stanc_s"], 3)
        self.assertEqual(row["cmdstan_build_s"], 5)
        self.assertEqual(row["stanli_estimated_s"], 2 + .5 + 5000 * 250000 / 1e9)
        self.assertEqual(row["cmdstan_estimated_s"], 3 + 5 + 5000 * 500000 / 1e9)
        self.assertEqual(record["preparation_s"], [.25, .75])
        self.assertEqual(set(record["setup"]), {"stanli_compile", "cmdstan_stanc", "cmdstan_build"})
        self.assertNotIn("sampling", record)
        self.assertTrue(all("sample" not in event["argv"] for event in self.events))
        build = record["setup"]["cmdstan_build"]["argv"]
        self.assertEqual(build[0], "make")
        self.assertIn("STANCFLAGS=--O1", build)
        self.assertEqual(json.loads((self.root / "m.result.json").read_text()), record)

    def test_ordinary_build_failure_preserves_accepted_gradients_and_stanli_proxy(self):
        self.failures["m/cmdstan-build"] = "failed"
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["row"]["paired_rounds"], 2)
        self.assertEqual(record["row"]["paired_speedup"], 2)
        self.assertIn("stanli_estimated_s", record["row"])
        self.assertNotIn("cmdstan_build_s", record["row"])
        self.assertNotIn("cmdstan_estimated_s", record["row"])
        self.assertEqual(record["setup"]["cmdstan_build"]["status"], "failed")

    def test_timed_out_build_is_censored_without_an_estimate(self):
        self.failures["m/cmdstan-build"] = "timeout"
        record = self.measure()
        self.assertEqual(record["status"], "censored")
        self.assertNotIn("cmdstan_estimated_s", record["row"])
        self.assertIn("timeout", record["row"]["note"])

    def test_partial_preparation_does_not_create_a_survivor_median(self):
        self.preparation = iter(["0.25 s", "nan s"])
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["preparation_s"], [.25])
        self.assertNotIn("stanli_prep_s", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])
        self.assertIn("cmdstan_estimated_s", record["row"])
        self.assertEqual(record["row"]["paired_rounds"], 2)

    def test_numerical_failure_has_no_gradient_proxy_but_keeps_setup_measurements(self):
        self.mismatch = 2
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertRegex(record["row"]["note"], "point 2")
        self.assertNotIn("paired_speedup", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])
        self.assertNotIn("cmdstan_estimated_s", record["row"])
        self.assertEqual(record["row"]["cmdstan_build_s"], 5)

    def test_gradient_commands_carry_the_window_and_point_settings(self):
        self.measure()
        timed = [e["argv"] for e in self.events if "--timed" in e["argv"]]
        self.assertEqual(len(timed), 4)
        for argv in timed:
            self.assertEqual(argv[argv.index("--timed"):argv.index("--timed") + 7],
                             ["--timed", "--warmup-ms", "20", "--measure-ms", "40",
                              "--points", "3"])

    def test_reference_points_replace_the_point_count_in_both_engines(self):
        frozen = self.root / "m.points.json"
        reference = dict(source="reference_run", gradient_count=1234,
                         reference=self.root / "m.reference.json", points=frozen)
        record = self.measure(reference=reference)
        timed = [e["argv"] for e in self.events if "--timed" in e["argv"]]
        self.assertEqual(len(timed), 4)
        for argv in timed:
            start = argv.index("--timed")
            self.assertEqual(argv[start:start + 7],
                             ["--timed", "--warmup-ms", "20", "--measure-ms", "40",
                              "--points-file", str(frozen)])
            self.assertNotIn("--points", argv)
        row = record["row"]
        self.assertEqual((row["point_source"], row["gradient_count"]), ("reference_run", 1234))
        self.assertEqual(row["stanli_estimated_s"], 2 + .5 + 1234 * 250000 / 1e9)
        self.assertEqual(row["cmdstan_estimated_s"], 3 + 5 + 1234 * 500000 / 1e9)
        self.assertEqual(record["inputs"], dict(stan="test-hash", data="test-hash",
                                                point_source="reference_run", gradient_count=1234,
                                                reference_sha256="test-hash", points_sha256="test-hash"))

    def test_fixed_points_are_recorded_as_such(self):
        record = self.measure()
        self.assertEqual(record["row"]["point_source"], "fixed")
        self.assertEqual(record["inputs"], dict(stan="test-hash", data="test-hash",
                                                point_source="fixed", gradient_count=5000))
        self.assertIn("point_source", COLS)
        self.assertIn("gradient_count", COLS)

    def test_records_keep_timings_and_scaled_errors_but_not_values(self):
        record = self.measure()
        self.assertEqual(record["status"], "ok")
        self.assertNotIn('"values"', json.dumps(record))
        for gradient in record["gradients"]:
            self.assertEqual(gradient["width"], 2)
            self.assertEqual(gradient["scaled_errors"], [0, 0, 0])
            for engine in ("stanli", "cmdstan"):
                self.assertEqual([sorted(p) for p in gradient[engine]["points"]],
                                 [["batch", "elapsed_ns", "finite", "iterations", "point"]] * 3)
        self.assertEqual(record["row"]["params"], 1)

    def test_points_non_finite_in_both_engines_are_dropped_and_recorded(self):
        self.dropped = {"stanli": (1,), "cmdstan": (1,)}
        record = self.measure()
        self.assertEqual(record["status"], "ok")
        self.assertEqual((record["row"]["points_kept"], record["row"]["points_dropped"]), (2, 1))
        self.assertTrue(all(g["dropped_points"] == [1] and g["kept_points"] == [0, 2]
                            for g in record["gradients"]))
        self.assertEqual(record["row"]["params"], 1)
        self.assertEqual(record["row"]["stanli_ns_grad"], 250000)
        self.assertTrue(all(len(g["scaled_errors"]) == 2 for g in record["gradients"]))

    def test_a_point_non_finite_in_one_engine_fails_the_model(self):
        self.dropped = {"cmdstan": (1,)}
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertRegex(record["row"]["note"], "point 1")
        self.assertNotIn("paired_speedup", record["row"])
        self.assertEqual(record["row"]["cmdstan_build_s"], 5)

    def test_every_round_must_keep_the_same_points(self):
        self.dropped = {(1, "stanli"): (2,), (1, "cmdstan"): (2,)}
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertRegex(record["row"]["note"], "points differ")
        self.assertNotIn("stanli_ns_grad", record["row"])

    def test_source_compile_failure_keeps_event_without_setup_estimate(self):
        self.failures["m/stanli-mir"] = "failed"
        record = self.measure()
        self.assertEqual(len(self.events), 1)
        self.assertEqual(record["setup"]["stanli_compile"]["status"], "failed")
        self.assertNotIn("stanli_compile_s", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])

    def test_partial_gradient_rounds_cannot_supply_an_estimate(self):
        self.failures["m/gradient/1/cmdstan"] = "timeout"
        record = self.measure()
        self.assertEqual(record["status"], "censored")
        self.assertEqual(len(record["gradients"]), 1)
        self.assertNotIn("stanli_ns_grad", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])
        self.assertNotIn("cmdstan_estimated_s", record["row"])
        self.assertEqual(record["row"]["cmdstan_build_s"], 5)

    def test_sampling_options_are_no_longer_operational(self):
        from harnesses.corpus_bench import main
        for option in ("--sampling", "--sample-timeout", "--cmdstan-runtime-multiple",
                       "--seeds", "--iter-warmup", "--iter-sampling", "--run"):
            with self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as raised:
                main(["unused-cmdstan", "unused-pdb", "unused.tsv", option])
            self.assertEqual(raised.exception.code, 2)


class ReferenceInputTests(unittest.TestCase):
    SOURCE = "parameters { real x; }\nmodel { x ~ normal(0, 1); }\n"

    def setUp(self):
        import benchmark_points as bp
        self.bp = bp
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.stan, self.data = self.root / "m.stan", self.root / "m.json"
        self.stan.write_text(self.SOURCE)
        self.data.write_text("{}")
        self.other = self.root / "o.stan"
        self.other.write_text("parameters { real y; }\nmodel { y ~ normal(0, 1); }\n")
        phases = lambda a, b: dict(warmup=a, sampling=b)
        self.doc = dict(schema=bp.SCHEMA, settings=dict(points=3, warmup=2, draws=2),
                        models={
                            "m": dict(source="reference_run", stan_sha256=bp.sha256(self.stan),
                                      data_sha256=bp.sha256(self.data), n_params=1,
                                      points=[[0.5], [1.5], [-0.5]], iterations=[0, 1, 3],
                                      gradients=dict(warmup=10, sampling=20, total=30),
                                      divergences=phases(0, 0), max_treedepth=phases(0, 0)),
                            "o": dict(source="fixed", stan_sha256=bp.sha256(self.other),
                                      data_sha256=bp.sha256(self.data), reason="reference run timed out")})

    def select(self, doc=None, points=3, **inputs):
        from harnesses.corpus_bench import reference_inputs
        inputs = inputs or {"m": (self.stan, self.data)}
        return reference_inputs(inputs, doc or self.doc, points, self.root / "frozen")

    def test_a_reference_model_gets_frozen_points_and_its_gradient_count(self):
        found = self.select(m=(self.stan, self.data), o=(self.other, self.data))
        self.assertEqual(sorted(found), ["m", "o"])
        m = found["m"]
        self.assertEqual((m["source"], m["gradient_count"]), ("reference_run", 30))
        self.assertEqual(json.loads(m["points"].read_text()), [[0.5], [1.5], [-0.5]])
        self.assertEqual(m["points"].parent, self.root / "frozen")
        self.assertEqual(json.loads(m["reference"].read_text())["iterations"], [0, 1, 3])

    def test_a_fallback_model_keeps_the_fixed_points_and_the_default_count(self):
        o = self.select(o=(self.other, self.data))["o"]
        self.assertEqual((o["source"], o["gradient_count"], o["points"]), ("fixed", 5000, None))
        self.assertEqual(json.loads(o["reference"].read_text())["reason"], "reference run timed out")

    def test_a_changed_source_or_data_file_is_an_error(self):
        self.stan.write_text(self.SOURCE + "\n")
        with self.assertRaisesRegex(ValueError, "stan source changed"):
            self.select()
        self.stan.write_text(self.SOURCE)
        self.data.write_text('{"x": 1}')
        with self.assertRaisesRegex(ValueError, "data changed"):
            self.select()

    def test_a_model_missing_from_the_file_is_an_error(self):
        self.doc["models"].clear()
        with self.assertRaisesRegex(ValueError, "no entry"):
            self.select()

    def test_the_point_count_must_match_the_run(self):
        with self.assertRaisesRegex(ValueError, "3 points per model but --points is 10"):
            self.select(points=10)


class CompilerSelectionTests(unittest.TestCase):
    def test_shipped_pipeline_and_reference_flags_are_independent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "m.stan"
            source.write_text("parameters { real x; } model { x ~ normal(0, 1); }")
            args = SimpleNamespace(vectorize_probe=root / "probe", stanc=root / "stanc",
                stancflags="--O1 --vectorize-loops", build_timeout=900,
                cmdstan=root / "cmdstan", bench=root / "bench")
            runner = mock.Mock()
            runner.run.return_value = dict(status="ok", elapsed_s=1)
            with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]):
                commands, _ = build_model("m", source, root / "data.json",
                                           root / "build", runner, args)
            calls = runner.run.call_args_list
            self.assertEqual(calls[0].args[1][:3],
                             [args.vectorize_probe, "--vectorize-loops", "on"])
            self.assertEqual(calls[1].args[1][0], args.stanc)
            self.assertEqual(calls[1].args[1][-2:], ["--O1", "--vectorize-loops"])
            self.assertEqual(commands["stanli"][0], args.bench)


class FastMathModeTests(SetupEstimateTests):
    def run_with(self, fast_math):
        self.args.fast_math = fast_math
        with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]):
            self.measure()
        return [[str(part) for part in event["argv"]] for event in self.events]

    def test_default_mode_never_passes_the_flag(self):
        for argv in self.run_with(False):
            self.assertNotIn("--fast-math", argv)

    def test_fast_mode_reaches_only_the_stanli_commands(self):
        commands = self.run_with(True)
        probe = [a for a in commands if a[0] == str(self.args.vectorize_probe)]
        self.assertEqual(len(probe), 1)
        self.assertIn("--fast-math", probe[0])
        for argv in commands:
            if argv[0] == str(self.args.bench):
                self.assertEqual(argv[-1], "--fast-math")
                self.assertIn(argv[3], ("--timed", "--prep"))
            elif argv[0] != str(self.args.vectorize_probe):
                self.assertNotIn("--fast-math", argv)
        self.assertTrue(any(a[0] == str(self.args.bench) for a in commands))

    def test_mode_is_part_of_the_run_identity_only_when_on(self):
        base = dict(rounds=2, warmup_ms=1, points=4, resume=False, output="x")
        self.assertEqual(run_config(SimpleNamespace(**base, fast_math=False)),
                         run_config(SimpleNamespace(**base)))
        self.assertNotIn("fast_math", run_config(SimpleNamespace(**base, fast_math=False)))
        self.assertIs(run_config(SimpleNamespace(**base, fast_math=True))["fast_math"], True)
        self.assertEqual(run_config(SimpleNamespace(**base))["points"], 4)


class CorpusInventoryTests(unittest.TestCase):
    def test_educational_cases_are_default_and_selectable_without_posteriordb(self):
        cases = benchmark_cases(pathlib.Path("missing/posterior_database"))
        self.assertEqual(len(cases), 223)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "educational")), 13)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "rethinking")), 62)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "brms")), 124)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "cogmod")), 22)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "ctsem")), 2)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "teaching")), 199)
        for source, data in cases.values():
            self.assertTrue(source.is_file())
            self.assertTrue(data.is_file())
        self.assertEqual(benchmark_cases(pathlib.Path("missing"), "posteriordb"), {})
        with self.assertRaises(ValueError):
            benchmark_cases(pathlib.Path("missing"), "typo")

    def test_posteriordb_keeps_first_dataset_and_can_combine_collections(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "posteriors").mkdir()
            for name, data in [("a", "first"), ("b", "second")]:
                (root / "posteriors" / (name + ".json")).write_text(json.dumps(
                    {"model_name": "example", "data_name": data}))
            cases = benchmark_cases(root)
            self.assertEqual(len(cases), 224)
            self.assertEqual(cases["example"][1].name, "first.json.zip")
            self.assertEqual(len(benchmark_cases(root, "posteriordb")), 1)

    def test_plain_and_zipped_data_materialize_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            plain, zipped, result = root / "data.json", root / "data.json.zip", root / "out.json"
            payload = b'{"N": 2, "y": [0.25, 0.75]}\n'
            plain.write_bytes(payload)
            with zipfile.ZipFile(zipped, "w") as archive:
                archive.writestr("data.json", payload)
            for source in [plain, zipped]:
                materialize_data(source, result)
                self.assertEqual(result.read_bytes(), payload)



class TriageTests(unittest.TestCase):
    def render(self, rows):
        from harnesses.triage_bench import main
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "bench.tsv"
            with path.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t")
                writer.writeheader()
                writer.writerows(rows)
            output = io.StringIO()
            with mock.patch.object(sys, "argv", ["triage_bench.py", str(path), "--all"]), \
                    contextlib.redirect_stdout(output):
                main()
            return output.getvalue()

    def test_current_estimates_use_paired_ratio_and_keep_missing_estimates_visible(self):
        row = dict(model="valid", stanli_ns_grad=100, cmdstan_ns_grad=200,
                   paired_speedup=1.25, gradient_count=5000,
                   stanli_estimated_s=.1, cmdstan_estimated_s=.2, note="")
        missing = dict(row, model="missing", stanli_estimated_s="", cmdstan_estimated_s="",
                       note="ordinary build failed")
        result = self.render([row, missing])
        self.assertIn("reference-run gradients (proxy, not HMC time)", result)
        self.assertNotIn("Historical sampling", result)
        measured = next(line.split() for line in result.splitlines() if line.lstrip().startswith("valid "))
        self.assertEqual(measured[-4:], ["1.25x", "0.10", "0.20", "2.00x"])
        absent = next(line.split() for line in result.splitlines() if line.lstrip().startswith("missing "))
        self.assertEqual(absent[-3:], ["-", "-", "-"])
        self.assertIn("ordinary build failed", result)

    def test_historical_sampling_remains_explicitly_labeled(self):
        result = self.render([dict(model="old", stanli_ns_grad=100, cmdstan_ns_grad=200,
                                   stanli_sample_s=3, cmdstan_sample_s=9, note="")])
        self.assertIn("Historical sampling seconds", result)
        self.assertNotIn("Estimated seconds", result)
        measured = next(line.split() for line in result.splitlines() if line.lstrip().startswith("old "))
        self.assertEqual(measured[-4:], ["2.00x", "3.00", "9.00", "3.00x"])



class HistoricalResultsTests(unittest.TestCase):
    def report(self):
        return json.loads((REPO / "tests/educational/pareto-benchmark-results.json").read_text())

    def render(self, report):
        from tools.corpus_table import render_historical_sampling
        result = io.StringIO()
        with contextlib.redirect_stdout(result):
            render_historical_sampling(report)
        return result.getvalue()

    def test_results_page_matches_retained_measurements(self):
        rendered = self.render(self.report())
        page = (REPO / "notes/performance/benchmark-history.md").read_text()
        body = page.split("<!-- educational-results:start -->\n")[1].split(
            "<!-- educational-results:end -->")[0]
        self.assertEqual(rendered, body)
        self.assertEqual(rendered.count("| `aalto_"), 13)
        self.assertIn("compiled CmdStan run", rendered)

    def test_gradient_results_page_matches_raw_suite_rows(self):
        from tools.corpus_table import load_rows, render_gradients
        rows, col = load_rows(REPO / "docs/internal/artifacts/educational-bench-o1.tsv")
        result = io.StringIO()
        with contextlib.redirect_stdout(result):
            render_gradients(rows, col)
        body = (REPO / "notes/performance/benchmark-history.md").read_text().split(
            "<!-- educational-gradients:start -->\n")[1].split(
            "<!-- educational-gradients:end -->")[0]
        self.assertEqual(result.getvalue(), body)
        self.assertEqual(len(rows), 13)
        # Nonfinite timing data cannot become a published speedup.
        invalid = [["m", "1", "nan", "100", ""]]
        columns = ["model", "params", "stanli_ns_grad", "cmdstan_ns_grad", "note"]
        with self.assertRaises(ValueError), contextlib.redirect_stdout(io.StringIO()):
            render_gradients(invalid, lambda r, c: r[columns.index(c)])

    def test_missing_model_or_failed_performance_cannot_be_published(self):
        report = self.report()
        missing = copy.deepcopy(report)
        del missing["models"]["aalto_bern"]
        with self.assertRaises(ValueError):
            self.render(missing)
        failed = copy.deepcopy(report)
        # The renderer must recompute from observations, not trust a stale
        # saved speedup/pass cell after observations have changed.
        failed["models"]["aalto_gpareto"]["benchmark"]["seconds"]["stanli"] = [100] * 5
        with self.assertRaises(ValueError):
            self.render(failed)



class BenchmarkCatalogTests(unittest.TestCase):
    def test_complete_rows_rank_by_unrounded_ratio_and_incomplete_rows_stay_separate(self):
        from tools.corpus_table import _catalog_tables
        rows = [
            {"model": "middle", "paired_speedup": 1.2343},
            {"model": "failed", "note": "gradient build failed"},
            {"model": "highest", "paired_speedup": 2.0},
            {"model": "slightly_higher", "paired_speedup": 1.2344},
        ]
        details = {row["model"]: {"status": "failed" if row["model"] == "failed" else "ok"}
                   for row in rows}
        complete, incomplete = _catalog_tables(rows, details, 10).split("**Incomplete results**")
        self.assertLess(complete.index("`highest`"), complete.index("`slightly_higher`"))
        self.assertLess(complete.index("`slightly_higher`"), complete.index("`middle`"))
        self.assertNotIn("`failed`", complete)
        self.assertIn("`failed`", incomplete)

    def setUp(self):
        from tools.corpus_table import render_catalog
        from benchmark_artifact_fixture import artifact_fixture
        self.render_catalog = render_catalog
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.paths = (root / "summary.tsv", root / "records.json.gz", root / "manifest.json")
        self.manifest, self.records, self.rows = artifact_fixture()

    def render(self):
        from benchmark_artifact_fixture import write_artifacts
        write_artifacts(*self.paths, self.rows, self.records, self.manifest)
        return self.render_catalog(*self.paths)

    def update_row(self, **values):
        self.rows[0].update(values)
        self.records[0]["row"].update(values)

    def test_proxy_uses_all_setup_terms_and_five_thousand_gradients(self):
        rendered = self.render()
        self.assertIn("1.42x", rendered)
        self.assertNotIn("| 2x |", rendered)
        self.assertIn("| Gradients |", rendered)
        self.assertIn("| 1.42x | 5k | 0.122 | 2.2 |", rendered)
        self.assertIn("Full sampling is not run", rendered)
        self.assertIn("5,000", rendered)
        complete, incomplete = rendered.split("**Incomplete results**")
        self.assertIn("`good`", complete)
        self.assertNotIn("`failed`", complete)
        self.assertNotIn("±", complete)
        self.assertNotIn("Notes", complete)
        self.assertIn("`failed` | — | — | — | — | — | failed; gradient build failed", incomplete)

    def test_missing_failed_row_is_not_complete_inventory(self):
        self.rows.pop()
        self.records.pop()
        with self.assertRaisesRegex(ValueError, "inventory"):
            self.render()

    def test_duplicate_tsv_rows_are_rejected(self):
        self.rows.append(self.rows[0])
        with self.assertRaisesRegex(ValueError, "Duplicate model"):
            self.render()

    def test_duplicate_raw_records_are_rejected(self):
        self.records.append(self.records[0])
        with self.assertRaisesRegex(ValueError, "inventory"):
            self.render()

    def test_run_id_mismatch_is_rejected(self):
        self.update_row(run_id="other")
        with self.assertRaisesRegex(ValueError, "run identity"):
            self.render()

    def test_source_hash_drift_is_rejected(self):
        self.records[0]["inputs"]["stan"] = "different"
        with self.assertRaisesRegex(ValueError, "input hashes"):
            self.render()

    def test_tsv_must_agree_with_raw_row(self):
        self.rows[0]["paired_speedup"] = 99
        with self.assertRaisesRegex(ValueError, "TSV/raw"):
            self.render()

    def test_raw_pairs_must_support_summary(self):
        self.update_row(paired_speedup=99)
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_incomplete_pairs_are_rejected(self):
        self.records[0]["gradients"].pop()
        with self.assertRaisesRegex(ValueError, "Incomplete paired"):
            self.render()

    def test_failed_ordinary_build_preserves_gradients_and_stanli_estimate(self):
        self.records[0]["status"] = "failed"
        self.records[0]["setup"]["cmdstan_build"]["status"] = "failed"
        self.update_row(cmdstan_build_s="", cmdstan_estimated_s="", note="ordinary build failed")
        rendered = self.render()
        self.assertIn("1.42x", rendered)
        self.assertIn("| 0.122 | — | — | failed; ordinary build failed", rendered)

    def test_partial_preparation_preserves_gradient_and_reference_estimate(self):
        self.records[0]["status"] = "censored"
        self.records[0]["preparation_s"].pop()
        self.update_row(stanli_prep_s="", stanli_estimated_s="", note="prepare timed out")
        self.assertIn("| — | 2.2 | — | censored; prepare timed out", self.render())

    def test_failed_setup_cannot_supply_a_duration(self):
        self.records[0]["setup"]["cmdstan_build"]["status"] = "failed"
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_successful_setup_requires_exit_status_zero(self):
        self.records[0]["setup"]["cmdstan_build"]["returncode"] = 1
        with self.assertRaisesRegex(ValueError, "exit status zero"):
            self.render()

    def test_plain_tsv_cli_cannot_render_current_unvalidated_estimates(self):
        from tools.corpus_table import main
        self.render()
        for flags in ([], ["--gradients"], ["--o1vec"]):
            with self.subTest(flags=flags), mock.patch.object(sys, "argv", ["corpus_table", *flags, str(self.paths[0])]):
                with self.assertRaisesRegex(SystemExit, "raw evidence validation"):
                    main()

    def test_setup_phase_cannot_be_substituted_with_gradient_driver_build(self):
        self.records[0]["setup"]["cmdstan_build"]["phase"] = "good/gradient-driver-build"
        with self.assertRaisesRegex(ValueError, "Invalid setup event"):
            self.render()

    def test_proxy_is_recomputed_not_trusted(self):
        self.update_row(stanli_estimated_s=.12 + 2000 * 350 / 1e9)
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_wrong_budget_or_old_sampling_protocol_is_rejected(self):
        for field, value in (("gradient_budget", 2000), ("sampling", True), ("filter", "good")):
            config = copy.deepcopy(self.manifest["identity"]["config"])
            self.manifest["identity"]["config"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "unfiltered v5"):
                self.render()
            self.manifest["identity"]["config"] = config
        self.manifest["identity"]["protocol"] = "stanli-corpus-v3"
        with self.assertRaises(ValueError):
            self.render()

    def test_nonfinite_measurement_is_rejected(self):
        self.update_row(stanli_ns_grad=float("nan"))
        with self.assertRaisesRegex(ValueError, "Invalid stanli_ns_grad"):
            self.render()

    def test_numerical_mismatch_is_not_timing_evidence(self):
        pair = self.records[0]["gradients"][0]
        pair["scaled_errors"][1] = 2e-9
        pair["max_scaled_error"] = 2e-9
        with self.assertRaisesRegex(ValueError, "numerical gate"):
            self.render()

    def test_recorded_errors_must_be_consistent(self):
        pair = self.records[0]["gradients"][1]
        for label, edit in (("max differs", lambda: pair.update(max_scaled_error=0)),
                            ("too few", lambda: pair["scaled_errors"].pop()),
                            ("negative", lambda: pair["scaled_errors"].__setitem__(0, -1.0)),
                            ("nan", lambda: pair["scaled_errors"].__setitem__(0, float("nan"))),
                            ("text", lambda: pair["scaled_errors"].__setitem__(0, "0")),
                            ("missing", lambda: pair.pop("scaled_errors"))):
            saved = copy.deepcopy(pair)
            edit()
            with self.subTest(label), self.assertRaisesRegex(ValueError, "numerical gate"):
                self.render()
            pair.clear()
            pair.update(saved)

    def test_published_records_carry_no_values(self):
        self.records[0]["gradients"][0]["stanli"]["points"][0]["values"] = [-1.0, 2.0]
        with self.assertRaisesRegex(ValueError, "Invalid raw gradient trial"):
            self.render()

    def test_width_must_match_the_parameter_count(self):
        self.records[0]["gradients"][2]["width"] = 5
        with self.assertRaisesRegex(ValueError, "widths differ"):
            self.render()
        self.records[0]["gradients"][2]["width"] = 2
        self.update_row(params=7)
        with self.assertRaisesRegex(ValueError, "parameter count"):
            self.render()

    def test_published_means_are_recomputed_from_every_point(self):
        point = self.records[0]["gradients"][2]["stanli"]["points"][1]
        point["elapsed_ns"] *= 2
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_point_lists_must_match_the_configured_count(self):
        self.records[0]["gradients"][0]["stanli"]["points"].pop()
        with self.assertRaisesRegex(ValueError, "Invalid raw gradient trial"):
            self.render()

    def test_short_point_window_is_rejected(self):
        self.records[0]["gradients"][0]["cmdstan"]["points"][0]["elapsed_ns"] = 1000
        with self.assertRaisesRegex(ValueError, "Invalid raw gradient trial"):
            self.render()

    def test_kept_and_dropped_points_must_match_the_raw_trials(self):
        self.records[0]["gradients"][0]["kept_points"] = [0, 1]
        with self.assertRaisesRegex(ValueError, "numerical gate"):
            self.render()

    def test_points_non_finite_in_both_engines_are_dropped_and_noted(self):
        from benchmark_artifact_fixture import artifact_fixture
        from tools.corpus_table import _catalog_notes
        self.manifest, self.records, self.rows = artifact_fixture(dropped=(1,))
        self.assertIn("1.42x", self.render())
        record = dict(self.records[0], status="failed")
        self.assertEqual(_catalog_notes(self.rows[0], record),
                         "failed; 1 of 3 points non-finite in both engines; dropped")
        self.assertEqual(_catalog_notes(self.rows[0], self.records[0]),
                         "1 of 3 points non-finite in both engines; dropped")
        self.assertEqual(_catalog_notes(self.rows[1], self.records[1]), "failed; gradient build failed")

    def test_kept_point_counts_in_the_summary_must_match_the_raw_trials(self):
        from benchmark_artifact_fixture import artifact_fixture
        self.manifest, self.records, self.rows = artifact_fixture(dropped=(1,))
        self.update_row(points_kept=3, points_dropped=0)
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_a_point_finite_in_one_engine_only_is_rejected(self):
        from benchmark_artifact_fixture import artifact_fixture
        self.manifest, self.records, self.rows = artifact_fixture(dropped=(1,))
        self.records[0]["gradients"][0]["cmdstan"]["points"][1] = dict(
            point=1, finite=True, iterations=1000000, elapsed_ns=100000000, batch=1)
        with self.assertRaisesRegex(ValueError, "numerical gate"):
            self.render()

    def v6(self, counts=None, **sources):
        from benchmark_artifact_fixture import artifact_fixture
        self.manifest, self.records, self.rows = artifact_fixture(
            point_sources=dict(dict(good="fixed", failed="fixed"), **sources), counts=counts)

    def test_v5_runs_carry_no_point_source_and_render_without_a_marker(self):
        rendered = self.render()
        self.assertNotIn("†", rendered)
        self.assertIn("+ 5,000 × its warm gradient time", rendered)

    def test_v6_marks_fallback_models_and_explains_the_marker(self):
        self.v6(good="reference_run", failed="fixed")
        rendered = self.render()
        self.assertIn("`failed` †", rendered)
        self.assertNotIn("`good` †", rendered)
        self.assertRegex(rendered, "† The reference run did not finish.*5,000 gradients")
        self.assertIn("1,000 warmup + 1,000 draw run", rendered)

    def test_v6_with_only_reference_runs_has_no_marker(self):
        self.v6(good="reference_run", failed="reference_run")
        self.assertNotIn("†", self.render())

    def test_v6_total_uses_the_models_own_gradient_count(self):
        self.v6(counts=dict(good=2000), good="reference_run")
        rendered = self.render()
        stanli = .1 + .02 + 2000 * 350 / 1e9
        cmdstan = .2 + 2 + 2000 * 700 / 1e9
        self.assertIn(f"| {stanli:.3g} | {cmdstan:.3g} |", rendered)
        self.v6(counts=dict(good=300000), good="reference_run")
        rendered = self.render()
        stanli = .1 + .02 + 300000 * 350 / 1e9
        cmdstan = .2 + 2 + 300000 * 700 / 1e9
        self.assertIn(f"| {stanli:.3g} | {cmdstan:.3g} |", rendered)

    def test_v6_estimates_that_ignore_the_count_are_rejected(self):
        self.v6(counts=dict(good=2000), good="reference_run")
        self.update_row(stanli_estimated_s=.1 + .02 + 5000 * 350 / 1e9)
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            self.render()

    def test_v6_requires_a_recorded_source_for_every_input(self):
        self.v6()
        del self.manifest["identity"]["inputs"]["good"]["point_source"]
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_v6_rejects_an_unknown_source(self):
        self.v6()
        self.manifest["identity"]["inputs"]["good"]["point_source"] = "prior"
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_the_summary_source_must_match_the_manifest(self):
        self.v6(good="reference_run")
        self.update_row(point_source="fixed")
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_the_summary_count_must_match_the_manifest(self):
        self.v6(good="reference_run")
        self.update_row(gradient_count=2001)
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_the_record_must_repeat_the_manifest_values(self):
        for key, value in (("point_source", "fixed"), ("gradient_count", 1999),
                           ("reference_sha256", "cd" * 32)):
            with self.subTest(key=key):
                self.v6(good="reference_run")
                self.records[0]["inputs"][key] = value
                with self.assertRaisesRegex(ValueError, "point source"):
                    self.render()

    def test_reference_models_need_a_points_hash_that_the_record_repeats(self):
        self.v6(good="reference_run")
        self.records[0]["inputs"]["points_sha256"] = "cd" * 32
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()
        self.v6(good="reference_run")
        del self.manifest["identity"]["inputs"]["good"]["points_sha256"]
        del self.records[0]["inputs"]["points_sha256"]
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_fixed_models_cannot_carry_a_points_hash(self):
        self.v6()
        self.manifest["identity"]["inputs"]["good"]["points_sha256"] = "ab" * 32
        self.records[0]["inputs"]["points_sha256"] = "ab" * 32
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_fixed_models_use_the_default_count(self):
        self.v6(counts=dict(good=2000))
        self.manifest["identity"]["inputs"]["good"]["gradient_count"] = 2000
        self.records[0]["inputs"]["gradient_count"] = 2000
        self.update_row(gradient_count=2000)
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_v6_runs_have_no_legacy_budget(self):
        self.v6()
        self.manifest["identity"]["config"]["gradient_budget"] = 20000
        with self.assertRaisesRegex(ValueError, "setup/gradient run"):
            self.render()

    def test_v5_runs_cannot_claim_a_point_source(self):
        self.manifest["identity"]["inputs"]["good"]["point_source"] = "fixed"
        with self.assertRaisesRegex(ValueError, "point source"):
            self.render()

    def test_v5_requires_a_point_count(self):
        del self.manifest["identity"]["config"]["points"]
        with self.assertRaisesRegex(ValueError, "Invalid gradient configuration: points"):
            self.render()

    def test_single_point_v4_runs_are_no_longer_accepted(self):
        self.manifest["identity"]["protocol"] = "stanli-corpus-v4"
        del self.manifest["identity"]["config"]["points"]
        with self.assertRaisesRegex(ValueError, "unfiltered v5"):
            self.render()

    def test_missing_artifacts_have_no_historical_fallback(self):
        with self.assertRaises(FileNotFoundError):
            self.render_catalog(*self.paths)
        self.assertTrue(all("corpus-performance" in str(path)
                            for path in self.render_catalog.__defaults__[:3]))


def _measured(trial):
    return [point for point in trial["points"] if point["finite"]]


def _scale_engine(record, engine, factor):
    import statistics
    for pair in record["gradients"]:
        for point in _measured(pair[engine]):
            point["elapsed_ns"] = int(point["elapsed_ns"] * factor)
    times = {name: [statistics.fmean(m["elapsed_ns"] / m["iterations"] for m in _measured(p[name]))
                    for p in record["gradients"]]
             for name in ("stanli", "cmdstan")}
    series = {"stanli_ns_grad": times["stanli"], "cmdstan_ns_grad": times["cmdstan"],
              "paired_speedup": [c / s for s, c in zip(times["stanli"], times["cmdstan"])]}
    row = record["row"]
    for field, values in series.items():
        row[field] = statistics.median(values)
        row[field + "_mad"] = statistics.median(abs(v - row[field]) for v in values)
    row["cmdstan_estimated_s"] = (row["cmdstan_stanc_s"] + row["cmdstan_build_s"]
                                  + 20000 * row["cmdstan_ns_grad"] / 1e9)
    row["stanli_estimated_s"] = (row["stanli_compile_s"] + row["stanli_prep_s"]
                                 + 20000 * row["stanli_ns_grad"] / 1e9)


class VectorizedFixture(unittest.TestCase):
    def setUp(self):
        from benchmark_artifact_fixture import artifact_fixture
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.default = artifact_fixture()
        self.default[0]["identity"]["toolchains"] = {"cmdstan": {"head": "cmdstan-head"}}
        self.vector = self.root / "vectorized"
        self.sources = {}
        self.add_source("old", ("good", "failed"), cmdstan_factor=2)
        self.provenance = dict(source_sha="a" * 40, binary_sha256="b" * 64,
            patch_sha256="c" * 64, optimization="O1 plus vectorize_loops",
            build_commands=["opam exec -- dune build"])
        self.combined = {"stanli_run_id": "current",
                         "cmdstan_runs": {"good": "old", "failed": "old"}}

    def add_source(self, run_id, models, cmdstan_factor=1, stanli_factor=3):
        from benchmark_artifact_fixture import artifact_fixture
        manifest, records, _ = artifact_fixture()
        manifest["run_id"] = run_id
        identity = manifest["identity"]
        identity["toolchains"] = {"cmdstan": {"head": "cmdstan-head"}}
        identity["config"]["stancflags"] = "--O1"
        identity["executables"]["bench"] = "older-bench-hash"
        records = [r for r in records if r["model"] in models]
        identity["inputs"] = {m: identity["inputs"][m] for m in models}
        if len(models) == 1:
            identity["config"]["filter"] = models[0]
        for record in records:
            record["row"]["run_id"] = run_id
            if record["status"] == "ok":
                _scale_engine(record, "cmdstan", cmdstan_factor)
                _scale_engine(record, "stanli", stanli_factor)
        self.sources[run_id] = (manifest, records)

    def write(self):
        from benchmark_artifact_fixture import write_artifacts
        default_dir = self.root / "default"
        default_dir.mkdir(exist_ok=True)
        manifest, records, rows = self.default
        write_artifacts(default_dir / "benchmark-summary.tsv", default_dir / "model-results.json.gz",
                        default_dir / "benchmark-manifest.json", rows, records, manifest)
        for run_id, (manifest, records) in self.sources.items():
            run = self.vector / "cmdstan-runs" / run_id
            run.mkdir(parents=True, exist_ok=True)
            write_artifacts(run / "benchmark-summary.tsv", run / "model-results.json.gz",
                            run / "benchmark-manifest.json",
                            [copy.deepcopy(r["row"]) for r in records], records, manifest)
        self.vector.mkdir(exist_ok=True)
        (self.vector / "compiler-provenance.json").write_text(json.dumps(self.provenance))
        (self.vector / "combined.json").write_text(json.dumps(self.combined))
        return default_dir

    def render(self):
        from tools.corpus_table import render_gradient_catalog
        default_dir = self.write()
        return render_gradient_catalog(self.vector, default_dir)


class VectorizedCatalogTests(VectorizedFixture):
    def test_stanli_side_comes_from_default_run_and_cmdstan_side_from_reused_run(self):
        rendered = self.render()
        self.assertIn("| `good` | 4x |", rendered)
        self.assertIn("| 0.122 | 2.21 | 18.1x |", rendered)
        self.assertIn("`failed` | — | — | — | — | — | failed; gradient build failed", rendered)
        self.assertIn("reused from run `old`", rendered)
        self.assertIn("2026-09-21", rendered)
        self.assertNotIn("paired", rendered.split("\n\n", 1)[0])

    def test_models_missing_from_the_reused_run_take_a_supplement_run(self):
        self.add_source("supp", ("good",), cmdstan_factor=3)
        self.combined["cmdstan_runs"]["good"] = "supp"
        rendered = self.render()
        self.assertIn("| `good` | 6x |", rendered)
        self.assertIn("run `supp`", rendered)

    def test_stanli_run_must_be_the_default_run(self):
        self.combined["stanli_run_id"] = "other"
        with self.assertRaisesRegex(ValueError, "Stanli run"):
            self.render()

    def test_every_default_model_needs_a_cmdstan_source(self):
        del self.combined["cmdstan_runs"]["failed"]
        with self.assertRaisesRegex(ValueError, "inventory"):
            self.render()

    def test_cmdstan_inputs_must_match_default_inputs(self):
        self.sources["old"][0]["identity"]["inputs"]["good"]["data"] = "changed"
        self.sources["old"][1][0]["inputs"]["data"] = "changed"
        with self.assertRaisesRegex(ValueError, "input"):
            self.render()

    def test_reference_points_in_the_default_run_need_matching_cmdstan_points(self):
        from benchmark_artifact_fixture import artifact_fixture
        self.default = artifact_fixture(point_sources=dict(good="reference_run", failed="fixed"))
        self.default[0]["identity"]["toolchains"] = {"cmdstan": {"head": "cmdstan-head"}}
        with self.assertRaisesRegex(ValueError, "differs"):
            self.render()

    def test_cmdstan_toolchain_must_match_default_run(self):
        self.sources["old"][0]["identity"]["toolchains"]["cmdstan"]["head"] = "newer"
        with self.assertRaisesRegex(ValueError, "toolchain"):
            self.render()

    def test_stanli_checkouts_may_differ_from_reused_run(self):
        self.default[0]["identity"]["toolchains"]["stanli_math"] = {"tracked_diff_sha256": "new"}
        self.sources["old"][0]["identity"]["toolchains"]["stanli_math"] = {"tracked_diff_sha256": "old"}
        self.assertIn("| `good` | 4x |", self.render())
        self.sources["old"][0]["identity"]["toolchains"]["clang_sha256"] = "other"
        with self.assertRaisesRegex(ValueError, "toolchain"):
            self.render()

    def test_gradient_protocol_must_match_default_run(self):
        self.sources["old"][0]["identity"]["config"]["measure_ms"] = 300
        with self.assertRaisesRegex(ValueError, "configuration differs"):
            self.render()
        self.sources["old"][0]["identity"]["config"]["measure_ms"] = 40
        self.sources["old"][0]["identity"]["threads"] = {"STAN_NUM_THREADS": "2"}
        with self.assertRaisesRegex(ValueError, "thread"):
            self.render()

    def test_compiler_provenance_must_match(self):
        for field, value in (("binary_sha256", "d" * 64), ("patch_sha256", ""),
                             ("source_sha", ""), ("build_commands", []),
                             ("optimization", "O1 only")):
            provenance = copy.deepcopy(self.provenance)
            self.provenance[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "compiler provenance"):
                self.render()
            self.provenance = provenance

    def test_current_optimized_flags_are_required(self):
        self.sources["old"][0]["identity"]["config"]["stancflags"] = ""
        with self.assertRaisesRegex(ValueError, "requires O1"):
            self.render()


class PublishVectorizedTests(VectorizedFixture):
    def test_filtered_supplement_run_publishes_with_its_filter_recorded(self):
        from tools.publish_corpus_bench import publish
        from benchmark_artifact_fixture import write_artifacts
        manifest, records = self.sources["old"]
        manifest["identity"]["config"]["filter"] = "good"
        manifest["identity"]["inputs"] = {"good": manifest["identity"]["inputs"]["good"]}
        records = records[:1]
        summary = self.root / "supp.tsv"
        run = pathlib.Path(str(summary) + ".run")
        run.mkdir()
        write_artifacts(summary, self.root / "unused.json.gz", run / "manifest.json",
                        [copy.deepcopy(records[0]["row"])], records, manifest)
        (run / "good.result.json").write_text(json.dumps(records[0]))
        (run / "events.jsonl").write_text("")
        with contextlib.redirect_stdout(io.StringIO()):
            publish(summary, self.root / "published")
        published = json.loads((self.root / "published/benchmark-manifest.json").read_text())
        self.assertEqual(published["identity"]["config"]["filter"], "good")
        self.assertIn("`good`", (self.root / "published/README.md").read_text())

    def test_publish_maps_each_model_to_the_last_run_that_has_it(self):
        from tools.publish_corpus_bench import publish_vectorized
        self.add_source("supp", ("good",), cmdstan_factor=3)
        default_dir = self.write()
        (self.vector / "combined.json").unlink()
        runs = [self.vector / "cmdstan-runs" / name for name in ("old", "supp")]
        publish_vectorized(self.vector, default_dir, runs)
        combined = json.loads((self.vector / "combined.json").read_text())
        self.assertEqual(combined, {"stanli_run_id": "current",
                                    "cmdstan_runs": {"failed": "old", "good": "supp"}})
        readme = (self.vector / "README.md").read_text()
        self.assertIn("| `good` | 6x |", readme)
        sums = (self.vector / "SHA256SUMS").read_text()
        self.assertIn("combined.json", sums)
        self.assertIn("README.md", sums)


if __name__ == "__main__":
    unittest.main()
