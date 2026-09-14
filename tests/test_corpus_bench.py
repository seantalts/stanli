#!/usr/bin/env python3
"""Focused tests for the corpus benchmark TSV serializer."""

import csv
import contextlib
import copy
import json
import tempfile
import zipfile
import io
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from harnesses.corpus_bench import (COLS, parse_grad_count, row_line,
                                     upgrade_header, benchmark_cases, materialize_data, selected_case)


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


class CorpusInventoryTests(unittest.TestCase):
    def test_educational_cases_are_default_and_selectable_without_posteriordb(self):
        cases = benchmark_cases(pathlib.Path("missing/posterior_database"))
        self.assertEqual(len(cases), 13)
        self.assertEqual(cases, benchmark_cases(pathlib.Path("missing"), "educational"))
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
            self.assertEqual(len(cases), 14)
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

    def test_refresh_only_selects_existing_rows(self):
        self.assertFalse(selected_case("aalto_bern", "", set(), True, {}))
        self.assertTrue(selected_case("aalto_bern", "aalto_", set(), True, {"aalto_bern": {}}))
        self.assertTrue(selected_case("aalto_bern", "", set(), False, {}))
        self.assertFalse(selected_case("aalto_bern", "", {"aalto_bern"}, False, {}))
        self.assertFalse(selected_case("aalto_bern", "schools", set(), False, {}))


class EducationalResultsTests(unittest.TestCase):
    def report(self):
        return json.loads((REPO / "tests/educational/pareto-benchmark-results.json").read_text())

    def render(self, report):
        from tools.corpus_table import render_educational
        result = io.StringIO()
        with contextlib.redirect_stdout(result):
            render_educational(report)
        return result.getvalue()

    def test_results_page_matches_retained_measurements(self):
        rendered = self.render(self.report())
        page = (REPO / "docs/benchmarks.md").read_text()
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
        body = (REPO / "docs/benchmarks.md").read_text().split(
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


if __name__ == "__main__":
    unittest.main()
