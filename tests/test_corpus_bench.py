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

from harnesses.corpus_bench import (COLS, GRADIENT_BUDGET, PROTOCOL, row_line,
    upgrade_header, Runner, PhaseFailure, parse_timing, check_pair,
    summarize_pairs, paired_order, open_run, measure_model,
    benchmark_cases, materialize_data, build_model)


class EnvironmentTests(unittest.TestCase):
    def test_compiler_experiment_overrides_are_rejected_before_measurement(self):
        from harnesses.corpus_bench import main
        for key in ("STANLI_BOUNDED_SPECIALIZATION", "STANLI_STRUCTURED_LOOPS",
                    "STANLI_SYMBOLIC_LANES", "STANLI_ISLAND_ALWAYS",
                    "STANLI_WA_FORCE_INTERP", "STANLI_PACKET_MATH"):
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


class TimingTests(unittest.TestCase):
    def timing(self, ns=250000000, iterations=1000, values=None):
        return dict(protocol="stanli-gradient-v2", iterations=iterations,
                    elapsed_ns=ns, batch=8, warmup_iterations=5000,
                    warmup_elapsed_ns=200000000, values=values or [-3.0, 2.0])

    def test_print_output_before_json_is_allowed(self):
        result = self.timing()
        self.assertEqual(parse_timing("Stan print\n" + json.dumps(result), 200, 250), result)

    def test_rejects_short_warmup_nonfinite_and_malformed_results(self):
        for change in (dict(warmup_elapsed_ns=1000), dict(iterations=0),
                       dict(values=[float("nan")]), dict(elapsed_ns=1),
                       dict(protocol="old"), dict(values=[])):
            with self.subTest(change=change), self.assertRaises(PhaseFailure):
                parse_timing(json.dumps(dict(self.timing(), **change)), 200, 250)
        with self.assertRaises(PhaseFailure):
            parse_timing("not JSON", 200, 250)

    def test_pair_checks_full_gradient_not_just_density(self):
        good = self.timing()
        with self.assertRaises(PhaseFailure):
            check_pair(dict(stanli=good, cmdstan=self.timing(values=[-3.0, 4.0])))
        with self.assertRaises(PhaseFailure):
            check_pair(dict(stanli=good, cmdstan=self.timing(values=[-3.0])))
        self.assertEqual(check_pair(dict(stanli=good, cmdstan=good)), 0)

    def test_balanced_order_and_paired_statistics(self):
        self.assertEqual([paired_order(i)[0] for i in range(6)],
                         ["stanli", "cmdstan"] * 3)
        pairs = [dict(stanli=self.timing(ns=s, iterations=1),
                      cmdstan=self.timing(ns=c, iterations=1))
                 for s, c in ((1, 2), (2, 2), (10, 30), (4, 4))]
        result = summarize_pairs(pairs)
        self.assertEqual(result["stanli_ns_grad"], 3)
        self.assertEqual(result["stanli_ns_grad_mad"], 1.5)
        self.assertEqual(result["paired_speedup"], 1.5)


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
            gradient_timeout=60, rounds=2, warmup_ms=200, measure_ms=250,
            cmdstan=self.root / "cmdstan", bench=self.root / "bench")
        self.events, self.failures = [], {}
        self.preparation = iter(["0.25 s", "0.75 s"])
        self.mismatch = False
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
        cmdstan = event["phase"].endswith("/cmdstan")
        return json.dumps(dict(protocol="stanli-gradient-v2", iterations=1000,
            elapsed_ns=500000000 if cmdstan else 250000000, batch=8,
            warmup_iterations=5000, warmup_elapsed_ns=200000000,
            values=[-3.0, 20.0 if self.mismatch and cmdstan else 2.0]))

    def measure(self):
        with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]), \
             mock.patch("harnesses.corpus_bench.sha", return_value="test-hash"):
            return measure_model("m", self.source, self.data, self.root,
                                 {"run_id": "test-run"}, self.runner, self.args)

    def test_proxy_includes_each_setup_component_but_not_driver_build(self):
        record = self.measure()
        row = record["row"]
        self.assertEqual(PROTOCOL, "stanli-corpus-v4")
        self.assertEqual(record["status"], "ok")
        self.assertEqual(GRADIENT_BUDGET, 20000)
        self.assertEqual(row["gradient_budget"], 20000)
        self.assertEqual(row["stanli_compile_s"], 2)
        self.assertEqual(row["stanli_prep_s"], .5)
        self.assertEqual(row["cmdstan_stanc_s"], 3)
        self.assertEqual(row["cmdstan_build_s"], 5)
        self.assertEqual(row["stanli_estimated_s"], 2 + .5 + 20000 * 250000 / 1e9)
        self.assertEqual(row["cmdstan_estimated_s"], 3 + 5 + 20000 * 500000 / 1e9)
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
        self.mismatch = True
        record = self.measure()
        self.assertEqual(record["status"], "failed")
        self.assertNotIn("paired_speedup", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])
        self.assertNotIn("cmdstan_estimated_s", record["row"])
        self.assertEqual(record["row"]["cmdstan_build_s"], 5)

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


class CorpusInventoryTests(unittest.TestCase):
    def test_educational_cases_are_default_and_selectable_without_posteriordb(self):
        cases = benchmark_cases(pathlib.Path("missing/posterior_database"))
        self.assertEqual(len(cases), 199)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "educational")), 13)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "rethinking")), 62)
        self.assertEqual(len(benchmark_cases(pathlib.Path("missing"), "brms")), 124)
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
            self.assertEqual(len(cases), 200)
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
                   paired_speedup=1.25, gradient_budget=20000,
                   stanli_estimated_s=.1, cmdstan_estimated_s=.2, note="")
        missing = dict(row, model="missing", stanli_estimated_s="", cmdstan_estimated_s="",
                       note="ordinary build failed")
        result = self.render([row, missing])
        self.assertIn("20,000 warm gradients (proxy, not HMC time)", result)
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
        page = (REPO / "docs/benchmark-history.md").read_text()
        body = page.split("<!-- educational-results:start -->\n")[1].split(
            "<!-- educational-results:end -->")[0]
        self.assertEqual(rendered, body)
        self.assertEqual(rendered.count("| `aalto_"), 13)
        self.assertIn("compiled CmdStan run", rendered)

    def test_gradient_results_page_matches_raw_suite_rows(self):
        from tools.corpus_table import load_rows, render_gradients
        rows, col = load_rows(REPO / "docs/educational-bench-o1.tsv")
        result = io.StringIO()
        with contextlib.redirect_stdout(result):
            render_gradients(rows, col)
        body = (REPO / "docs/benchmark-history.md").read_text().split(
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

    def test_proxy_uses_all_setup_terms_and_twenty_thousand_gradients(self):
        rendered = self.render()
        self.assertIn("1.42x ±", rendered)
        self.assertNotIn("2.00x", rendered)
        self.assertIn("| 0.127 | 2.214 |", rendered)
        self.assertIn("not measured HMC sampling time", rendered)
        self.assertIn("20,000", rendered)
        self.assertIn("`failed` | — | — | — | failed; gradient build failed", rendered)

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
        self.assertIn("1.42x ±", rendered)
        self.assertIn("| 0.127 | — | failed; ordinary build failed", rendered)

    def test_partial_preparation_preserves_gradient_and_reference_estimate(self):
        self.records[0]["status"] = "censored"
        self.records[0]["preparation_s"].pop()
        self.update_row(stanli_prep_s="", stanli_estimated_s="", note="prepare timed out")
        self.assertIn("| — | 2.214 | censored; prepare timed out", self.render())

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
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "unfiltered v4"):
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
        self.records[0]["gradients"][0]["cmdstan"]["values"] = [-10, 2]
        with self.assertRaisesRegex(ValueError, "numerical gate"):
            self.render()

    def test_missing_artifacts_have_no_historical_fallback(self):
        with self.assertRaises(FileNotFoundError):
            self.render_catalog(*self.paths)
        self.assertTrue(all("corpus-performance" in str(path)
                            for path in self.render_catalog.__defaults__))


class VectorizedCatalogTests(unittest.TestCase):
    def setUp(self):
        from tools.corpus_table import render_gradient_catalog
        from benchmark_artifact_fixture import artifact_fixture
        self.render_catalog = render_gradient_catalog
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.paths = [root / name for name in ("summary.tsv", "manifest.json",
                      "models.json.gz", "compiler.json", "default.json")]
        self.manifest, self.records, self.rows = artifact_fixture()
        self.baseline = copy.deepcopy(self.manifest)
        self.manifest["identity"]["config"]["stancflags"] = "--O1"
        self.provenance = dict(source_sha="a" * 40, binary_sha256="b" * 64,
            patch_sha256="c" * 64, optimization="O1 plus vectorize_loops",
            build_commands=["opam exec -- dune build"])

    def render(self):
        from benchmark_artifact_fixture import write_artifacts
        write_artifacts(self.paths[0], self.paths[2], self.paths[1],
                        self.rows, self.records, self.manifest)
        self.paths[3].write_text(json.dumps(self.provenance))
        self.paths[4].write_text(json.dumps(self.baseline))
        return self.render_catalog(*self.paths)

    def test_paired_ratio_mad_and_failures_are_retained(self):
        rendered = self.render()
        self.assertIn("1.42x ±", rendered)
        self.assertNotIn("2.00x", rendered)
        self.assertIn("`failed` | — | — | — | failed; gradient build failed", rendered)
        self.assertIn("| Stanli µs | CmdStan O1+vec µs |", rendered)

    def test_default_inputs_and_executables_must_match(self):
        for field in ("inputs", "bench", "vectorize_probe"):
            baseline = copy.deepcopy(self.baseline)
            if field == "inputs":
                self.baseline["identity"]["inputs"]["good"]["data"] = "changed"
            else:
                self.baseline["identity"]["executables"][field] = "changed"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "differ"):
                self.render()
            self.baseline = baseline

    def test_compiler_provenance_must_match(self):
        for field, value in (("binary_sha256", "d" * 64), ("patch_sha256", ""),
                             ("source_sha", ""), ("build_commands", []),
                             ("optimization", "O1 only")):
            provenance = copy.deepcopy(self.provenance)
            self.provenance[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "compiler provenance"):
                self.render()
            self.provenance = provenance

    def test_default_and_optimized_protocols_must_match(self):
        self.baseline["identity"]["config"]["rounds"] = 5
        with self.assertRaisesRegex(ValueError, "configuration differs"):
            self.render()

    def test_current_optimized_flags_are_required(self):
        self.manifest["identity"]["config"]["stancflags"] = ""
        with self.assertRaisesRegex(ValueError, "requires O1"):
            self.render()


if __name__ == "__main__":
    unittest.main()
