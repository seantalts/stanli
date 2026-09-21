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

from harnesses.corpus_bench import (COLS, parse_grad_count, row_line,
    upgrade_header, Runner, PhaseFailure, parse_timing, check_pair,
    summarize_pairs, paired_order, open_run, validate_draws, summarize_sampling,
    sampling_order, sampling_limit,
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

    def test_old_row_without_stanli_grads_round_trips(self):
        old_cols = [c for c in COLS if c != "stanli_grads"]
        text = ("\t".join(old_cols) + "\n"
                + "\t".join("m" if c == "model" else "" for c in old_cols)
                + "\n")
        old_row = next(csv.DictReader(io.StringIO(text), delimiter="\t"))
        self.assertEqual(self.parse(old_row)["stanli_grads"], "")


class UpgradeHeaderTests(unittest.TestCase):
    def test_noop_when_header_already_current(self):
        self.assertIsNone(upgrade_header(COLS, {}))

    def test_rewrites_old_header_to_current_cols(self):
        old_cols = [c for c in COLS if c != "stanli_grads"]
        rows = {"m": {**{c: "" for c in old_cols}, "model": "m"}}
        text = upgrade_header(old_cols, rows)
        header = text.splitlines()[0]
        self.assertEqual(header, "\t".join(COLS))


class ParseGradCountTests(unittest.TestCase):
    def test_extracts_count_from_stderr(self):
        stderr = ("stanli_run: 3 of 1000 draws could not produce generated "
                  "quantities, written as nan: bad\n"
                  "stanli_run: 12345 gradient evaluations\n")
        self.assertEqual(parse_grad_count(stderr), "12345")

    def test_empty_when_absent(self):
        self.assertEqual(parse_grad_count("stanli_run: boom\n"), "")


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
            for change in (dict(warmup_ms=100), dict(binary="new"), dict(data="new")):
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


class SamplingTests(unittest.TestCase):
    def test_relative_cap_uses_matching_reference_and_keeps_absolute_limit(self):
        self.assertEqual(sampling_order(0, 3), ("cmdstan", "stanli"))
        self.assertEqual(sampling_order(1, 3), ("cmdstan", "stanli"))
        self.assertEqual(sampling_order(0, None), ("stanli", "cmdstan"))
        self.assertEqual(sampling_limit("cmdstan", 900, 3, None), 900)
        self.assertEqual(sampling_limit("stanli", 900, 3, dict(status="ok", elapsed_s=12)), 36)
        self.assertEqual(sampling_limit("stanli", 900, 3, dict(status="ok", elapsed_s=400)), 900)
        self.assertEqual(sampling_limit("stanli", 900, None, None), 900)
        for reference in (None, dict(status="timeout"), dict(status="failed")):
            self.assertIsNone(sampling_limit("stanli", 900, 3, reference))

    def test_relative_cap_terminates_a_slow_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = Runner(pathlib.Path(temp))
            limit = sampling_limit("stanli", 900, 3, dict(status="ok", elapsed_s=0.05))
            result = runner.run("candidate", [sys.executable, "-c", "import time; time.sleep(10)"], limit)
            self.assertEqual(result["status"], "timeout")
            self.assertAlmostEqual(result["timeout_s"], 0.15)

    def test_rejects_incomplete_and_nonfinite_csv(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "draws.csv"
            path.write_text("# configuration\nlp__,mu\n-2,1\n-3,2\n# timing\n")
            validate_draws(path, 2)
            with self.assertRaises(PhaseFailure):
                validate_draws(path, 3)
            for text in ("", "mu\n1\n", "lp__,mu\n-2,nan\n", "lp__,mu\n-2\n"):
                path.write_text(text)
                with self.assertRaises(PhaseFailure):
                    validate_draws(path, 1)

    def test_one_censored_seed_prevents_survivor_average(self):
        events = [dict(engine=e, seed=s, status="ok", elapsed_s=s)
                  for e in ("stanli", "cmdstan") for s in (1, 2)]
        self.assertEqual(summarize_sampling(events, [1, 2])["stanli_sample_s"], 1.5)
        for status in ("timeout", "failed"):
            events[0]["status"] = status
            result = summarize_sampling(events, [1, 2])
            self.assertNotIn("stanli_sample_s", result)
            self.assertEqual(result["cmdstan_sample_s"], 1.5)


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
            with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]):
                commands, _ = build_model("m", source, root / "data.json",
                                           root / "build", runner, args)
            calls = runner.require.call_args_list
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
        self.render_catalog = render_catalog
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.paths = (root / "summary.tsv", root / "report.json", root / "manifest.json")
        self.manifest = {"run_id": "one-current-run", "started_utc": "2026-09-21T00:00:00Z",
                         "identity": {"config": {"corpus": "all", "filter": "", "sampling": True,
                                                "rounds": 6, "seeds": [1, 2],
                                                "iter_warmup": 1000, "iter_sampling": 1000},
                                      "inputs": {}}}
        self.rows, details = [], []
        for name in ("z_capped", "a_complete", "m_failed"):
            inputs = {"stan": name + "-source-hash", "data": name + "-data-hash"}
            self.manifest["identity"]["inputs"][name] = inputs
            row = {"model": name, "run_id": "one-current-run", "stanli_ns_grad": 100,
                   "cmdstan_ns_grad": 200, "paired_speedup": 1.8, "paired_speedup_mad": .1,
                   "paired_rounds": 6, "stanli_sample_s": 4, "cmdstan_sample_s": 10,
                   "cmdstan_build_s": 900, "note": ""}
            detail = {"model": name, "inputs": inputs, "status": "ok", "failure_reason": "",
                      "gradient": {key: row[key] for key in
                                   ("stanli_ns_grad", "cmdstan_ns_grad", "paired_speedup", "paired_speedup_mad")},
                      "engines": {}, "runs": []}
            for engine, elapsed in (("stanli", 4), ("cmdstan", 10)):
                detail["engines"][engine] = {"status": "complete", "median_s": elapsed,
                    "diagnostics": {"status": "complete", "screening_flag": False,
                                    "draws": 2000, "divergences": 0}}
                detail['runs'] += [{"engine": engine, "seed": seed, "status": "ok", "elapsed_s": elapsed + seed * 2 - 3}
                                   for seed in (1, 2)]
            if name == "z_capped":
                row['note'] = detail['failure_reason'] = "stanli_sample_timeout(0.6s, seed=2)"
                row['stanli_sample_s'] = ""
                detail['status'] = "censored"
                detail['engines']['stanli'] = {"status": "incomplete", "diagnostics": {"status": "incomplete"}}
                detail['runs'][1]['status'] = "timeout"
                detail['engines']['cmdstan']['diagnostics']['screening_flag'] = True
            elif name == "m_failed":
                row['note'] = detail['failure_reason'] = "density/gradient mismatch"
                detail['status'] = "failed"
                detail['gradient'] = {}
                detail['runs'] = []
                for key in ('stanli_ns_grad', 'cmdstan_ns_grad', 'paired_speedup', 'paired_speedup_mad',
                            'stanli_sample_s', 'cmdstan_sample_s'):
                    row[key] = ""
                detail['engines'] = {engine: {"status": "incomplete", "diagnostics": {"status": "incomplete"}}
                                     for engine in ('stanli', 'cmdstan')}
            self.rows.append(row)
            details.append(detail)
        self.report = {"run_id": "one-current-run", "manifest": self.manifest, "rows": details}

    def render(self):
        with self.paths[0].open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(self.rows[0]), delimiter='\t')
            writer.writeheader()
            writer.writerows(self.rows)
        self.paths[1].write_text(json.dumps(self.report))
        self.paths[2].write_text(json.dumps(self.manifest))
        return self.render_catalog(*self.paths)

    def cells(self):
        result = self.render()
        return {cells[0].strip('`'): cells for line in result.splitlines() if line.startswith('| `')
                for cells in [[cell.strip() for cell in line.split('|')[1:-1]]]}

    def test_one_complete_inventory_is_alphabetical_without_run_column(self):
        result = self.render()
        rows = self.cells()
        self.assertEqual(list(rows), ['a_complete', 'm_failed', 'z_capped'])
        self.assertNotIn('| Run |', result)
        self.assertIn('Run `one-current-run` (2026-09-21)', result)
        self.assertIn('6 paired gradient rounds and 2 sampling seeds', result)
        self.assertEqual(rows['a_complete'][1:4], ['1.80x ± 0.10', '4', '10'])
        self.assertNotIn('910', rows['a_complete'])  # Never add CmdStan compilation.

    def test_failures_caps_and_current_diagnostics_remain_visible(self):
        rows = self.cells()
        self.assertEqual(rows['m_failed'][1:4], ['—', '—', '—'])
        self.assertIn('density/gradient mismatch', rows['m_failed'][4])
        self.assertEqual(rows['z_capped'][2], '—')
        self.assertIn('stanli_sample_timeout(0.6s, seed=2)', rows['z_capped'][4])
        self.assertIn('CmdStan diagnostics flagged', rows['z_capped'][4])

    def test_incomparable_cli_warning_comes_from_current_draws_not_model_name(self):
        diagnostic = self.report['rows'][1]['engines']['cmdstan']['diagnostics']
        diagnostic.update(screening_flag=True, divergences=2000)
        self.assertIn('all retained CmdStan draws divergent', self.cells()['a_complete'][4])

    def test_missing_artifacts_never_fall_back_to_old_runs(self):
        with self.assertRaises(FileNotFoundError):
            self.render_catalog(*self.paths)
        self.assertTrue(all('output/corpus-performance/' in str(path)
                            for path in self.render_catalog.__defaults__))

    def test_missing_failed_row_is_rejected_by_manifest_inventory(self):
        self.rows = [row for row in self.rows if row['model'] != 'm_failed']
        self.report['rows'] = [row for row in self.report['rows'] if row['model'] != 'm_failed']
        with self.assertRaisesRegex(ValueError, 'inventories must match'):
            self.render()

    def test_duplicate_model_is_rejected(self):
        self.rows.append(copy.deepcopy(self.rows[0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate model'):
            self.render()

    def test_mixed_run_id_is_rejected(self):
        self.rows[0]['run_id'] = 'an-older-run'
        with self.assertRaisesRegex(ValueError, 'run identities differ'):
            self.render()

    def test_embedded_manifest_must_equal_external_manifest(self):
        self.report['manifest'] = copy.deepcopy(self.manifest)
        self.report['manifest']['started_utc'] = '2026-09-11T00:00:00Z'
        with self.assertRaisesRegex(ValueError, 'same run and manifest'):
            self.render()

    def test_input_identity_drift_is_rejected(self):
        self.report['rows'][0]['inputs'] = {'stan': 'changed-source', 'data': 'changed-data'}
        with self.assertRaisesRegex(ValueError, 'input hashes differ'):
            self.render()

    def test_filtered_corpus_is_not_a_complete_catalog(self):
        self.manifest['identity']['config']['corpus'] = 'teaching'
        with self.assertRaisesRegex(ValueError, 'unfiltered full-corpus'):
            self.render()

    def test_tsv_and_report_numerical_disagreement_is_rejected(self):
        self.report['rows'][1]['gradient']['paired_speedup'] = 9
        with self.assertRaisesRegex(ValueError, 'artifacts disagree'):
            self.render()

    def test_failed_seed_cannot_supply_completed_cli_median(self):
        self.report['rows'][1]['runs'][0]['status'] = 'timeout'
        with self.assertRaisesRegex(ValueError, 'Incomplete sampling seeds'):
            self.render()

    def test_partial_gradient_rounds_are_not_presented_as_full_measurement(self):
        self.rows[1]['paired_rounds'] = 2
        with self.assertRaisesRegex(ValueError, 'round count differs'):
            self.render()


if __name__ == "__main__":
    unittest.main()
