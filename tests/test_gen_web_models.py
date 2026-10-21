#!/usr/bin/env python3
"""Focused tests for the generated browser model catalog."""

import csv
import gzip
import json
import pathlib
import tempfile
import sys
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import gen_docs
from gen_web_models import benchmark_fields


class CurrentBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.directory = self.root / "output/corpus-performance"
        self.directory.mkdir(parents=True)
        from benchmark_artifact_fixture import artifact_fixture
        self.manifest, self.records, self.rows = artifact_fixture()
        self.row = self.rows[0]
        self.repo_patch = mock.patch.object(gen_docs, "REPO", self.root)
        self.repo_patch.start()
        self.addCleanup(self.repo_patch.stop)

    def write_fast(self):
        fast = self.root / "output/corpus-performance-fast"
        fast.mkdir(parents=True, exist_ok=True)
        rows = [dict(fast_row(r["model"], float(r["stanli_ns_grad"]) / 2, float(r["cmdstan_ns_grad"])))
                for r in self.rows if r.get("stanli_ns_grad")]
        with (fast / "benchmark-summary.tsv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        (fast / "benchmark-manifest.json").write_text(json.dumps(
            fast_manifest("2026-10-10", "f" * 40)))

    def write(self):
        from benchmark_artifact_fixture import write_artifacts
        write_artifacts(self.directory / "benchmark-summary.tsv",
                        self.directory / "model-results.json.gz",
                        self.directory / "benchmark-manifest.json",
                        self.rows, self.records, self.manifest)

    def test_current_headlines_use_validated_paired_ratios(self):
        self.write()
        catalog, rows, manifest = gen_docs.current_benchmark()
        self.assertIn("good", catalog)
        self.assertEqual(gen_docs.corpus_stats(rows), (1, self.row["paired_speedup"], 1))
        self.assertEqual(manifest["started_utc"][:10], "2026-09-21")

    def test_missing_current_artifacts_do_not_use_historical_measurements(self):
        (self.root / "docs/internal/artifacts").mkdir(parents=True)
        (self.root / "docs/internal/artifacts/corpus-bench.tsv").write_text("model\tpaired_speedup\nold\t99\n")
        with self.assertRaises(FileNotFoundError):
            gen_docs.current_benchmark()

    def test_mixed_run_or_summary_disagreement_cannot_become_a_headline(self):
        self.records[0]["row"]["run_id"] = "old"
        self.write()
        with self.assertRaisesRegex(ValueError, "run identity"):
            gen_docs.current_benchmark()
        self.records[0]["row"]["run_id"] = "current"
        self.records[0]["row"]["stanli_estimated_s"] = self.row["stanli_estimated_s"] = 99
        self.write()
        with self.assertRaisesRegex(ValueError, "artifacts disagree"):
            gen_docs.current_benchmark()

    def test_compute_preserves_reference_counts_and_vectorized_validation(self):
        self.write()
        (self.root / "docs/internal/artifacts").mkdir(parents=True)
        (self.root / "tests/educational/models").mkdir(parents=True)
        (self.root / "docs/internal/artifacts/verification.json").write_text(json.dumps({
            "pdb_case": {"status": "VERIFIED", "max_ulp": 0, "max_rel": 1e-13}}))
        (self.root / "docs/internal/artifacts/corpus-refs.json.gz").write_bytes(gzip.compress(json.dumps({
            "models": {"pdb_case": {"points": [{}, {}, {}]}}}).encode()))
        self.write_fast()
        with mock.patch.object(gen_docs, "render_gradient_catalog", return_value="validated vector table") as vector:
            stats = gen_docs.compute()
        vector.assert_called_once_with()
        self.assertEqual(stats["fast_geomean"], "2.00x")
        self.assertEqual(stats["fast_population"], "1")
        self.assertEqual(stats["benchmark_vectorized_catalog"], "validated vector table")
        self.assertEqual(stats["benchmark_models"], "2")
        self.assertEqual(stats["benchmark_stan_math"], "`" + "m" * 40 + "`")
        self.assertEqual(stats["benchmark_source"], "`" + "s" * 40 + "`")
        self.assertTrue(stats["benchmark_cmdstan"].startswith("`" + "c" * 40 + "`"))
        self.assertNotIn("benchmark_n_sampling", stats)
        self.assertEqual(stats["corpus_median"], "1.42x")
        self.assertEqual(stats["corpus_reference_models"], "1")
        self.assertEqual(stats["corpus_reference_points"], "3")
        self.assertEqual(stats["corpus_verified"], "1/1")
        self.assertEqual(stats["corpus_bitwise"], "1")

        # Setup failure does not erase accepted paired gradients from headlines.
        self.records[0]["status"] = "failed"
        self.records[0]["setup"]["cmdstan_build"]["status"] = "failed"
        for row in (self.row, self.records[0]["row"]):
            row.update(cmdstan_build_s="", cmdstan_estimated_s="", note="build failed")
        self.write()
        with mock.patch.object(gen_docs, "render_gradient_catalog", return_value="validated vector table"):
            stats = gen_docs.compute()
        self.assertEqual(stats["benchmark_models"], "2")
        self.assertNotIn("benchmark_n_sampling", stats)
        self.assertEqual(stats["corpus_n_grad"], "1")


def fast_row(model, stanli, cmdstan, paired=None, note=""):
    return {"model": model, "stanli_ns_grad": str(stanli), "cmdstan_ns_grad": str(cmdstan),
            "paired_speedup": str(paired if paired is not None else cmdstan / stanli), "note": note}


def fast_manifest(started, head, bench="b" * 64):
    return {"started_utc": started + "T01:02:03Z",
            "identity": {"executables": {"bench": bench},
                         "stanli_source": {"head": head}}}


class FastModeFigureTests(unittest.TestCase):
    def setUp(self):
        self.default = [fast_row("a", 1000, 2000), fast_row("b", 1000, 2000),
                        fast_row("c", 100, 150), fast_row("d", 50000, 60000),
                        fast_row("e", 40, 20)]
        self.fast = [fast_row("a", 10, 2000), fast_row("b", 2000, 2000, 0.99),
                     fast_row("c", 100, 150), fast_row("d", 500, 60000),
                     fast_row("e", 20, 20)]
        self.dm = fast_manifest("2026-10-20", "a" * 40)
        self.fm = fast_manifest("2026-10-10", "f" * 40)

    def figures(self):
        return gen_docs.fast_mode_figures(self.default, self.fast, self.dm, self.fm)

    def test_population_needs_both_runs_finite_positive_and_no_note(self):
        self.default.append(fast_row("only_default", 1, 1))
        self.fast.append(fast_row("only_fast", 1, 1))
        self.default.append(fast_row("noted", 1, 1, note="build failed"))
        self.fast.append(fast_row("noted", 1, 1))
        self.default.append(fast_row("inf", float("inf"), 1))
        self.fast.append(fast_row("inf", 1, 1))
        self.default.append(fast_row("zero", 1, 1))
        self.fast.append(fast_row("zero", 0, 1, paired=1))
        self.default.append(fast_row("blank", 1, 1))
        self.fast.append(dict(fast_row("blank", 1, 1), cmdstan_ns_grad=""))
        self.assertEqual(self.figures()["fast_population"], "5")

    def test_fast_over_default_geomean_median_and_threshold_counts(self):
        figures = self.figures()
        self.assertEqual(figures["fast_geomean"], "6.31x")
        self.assertEqual(figures["fast_median"], "2.00x")
        self.assertEqual(figures["fast_ge_two"], "3")
        self.assertEqual(figures["fast_ge_ten"], "2")
        self.assertEqual(figures["fast_ge_hundred"], "2")

    def test_cmdstan_geomean_and_win_counts_per_run(self):
        figures = self.figures()
        self.assertEqual(figures["fast_cmdstan_default"], "1.29x")
        self.assertEqual(figures["fast_cmdstan_fast"], "8.14x")
        self.assertEqual(figures["fast_beats_default"], "4")
        self.assertEqual(figures["fast_beats_fast"], "3")

    def test_top_table_is_the_strict_top_by_ratio_in_microseconds(self):
        self.default = [fast_row(f"m{i}", 1000 * (i + 1) ** 2, 1e6) for i in range(10)]
        self.fast = [fast_row(f"m{i}", 1000, 1e6) for i in range(10)]
        rows = self.figures()["fast_top_table"].splitlines()
        self.assertEqual(rows[0], "| model | default | fast | fast vs default |")
        self.assertEqual(rows[1], "| --- | ---: | ---: | ---: |")
        self.assertEqual(len(rows), 2 + 8)
        self.assertEqual(rows[2], "| `m9` | 100 | 1 | 100x faster |")
        self.assertEqual(rows[-1], "| `m2` | 9 | 1 | 9.0x faster |")

    def test_top_table_keeps_three_significant_figures(self):
        self.default = [fast_row("small", 40123, 1e6), fast_row("tiny", 18201, 1e6)]
        self.fast = [fast_row("small", 81, 1e6), fast_row("tiny", 72.4, 1e6)]
        rows = self.figures()["fast_top_table"].splitlines()
        self.assertEqual(rows[2], "| `small` | 40.1 | 0.081 | 495x faster |")
        self.assertEqual(rows[3], "| `tiny` | 18.2 | 0.0724 | 251x faster |")

    def test_top_two_inline_for_prose(self):
        self.assertEqual(self.figures()["fast_top_two"], "a 100x, d 100x")

    def test_slowest_model_is_named_with_its_slowdown(self):
        self.assertEqual(self.figures()["fast_slowest"],
                         "The slowest model in fast mode, `b`, is 2.00x slower.")

    def test_slowest_sentence_when_nothing_is_slower(self):
        self.fast = [dict(row, stanli_ns_grad=str(float(row["stanli_ns_grad"]) / 2))
                     for row in self.default]
        self.assertEqual(self.figures()["fast_slowest"], "No model is slower in fast mode.")

    def test_slowdown_that_rounds_to_one_is_not_printed_as_1_00x(self):
        self.default = [fast_row("x", 1000, 2000)]
        self.fast = [fast_row("x", 1001, 2000)]
        self.assertEqual(self.figures()["fast_slowest"],
                         "The slowest model in fast mode, `x`, is less than 1.01x slower.")

    def test_lead_for_same_binaries_and_source(self):
        self.dm = fast_manifest("2026-10-10", "f" * 40)
        figures = self.figures()
        self.assertIn("2026-10-10", figures["fast_lead"])
        self.assertIn("`ffffffff`", figures["fast_lead"])
        self.assertIn("same protocol, inputs and binaries as the default run", " ".join(figures["fast_lead"].split()))
        self.assertEqual(figures["fast_runs"], "2026-10-10, Stanli source `ffffffff`, default and fast mode from the same binaries")

    def test_lead_names_each_run_when_binaries_or_source_differ(self):
        figures = self.figures()
        lead = " ".join(figures["fast_lead"].split())
        self.assertNotIn("same protocol", lead)
        self.assertIn("fast mode run is from 2026-10-10 (Stanli source `ffffffff`)", lead)
        self.assertIn("default mode run is from 2026-10-20 (Stanli source `aaaaaaaa`)", lead)
        self.assertEqual(figures["fast_runs"], "fast mode 2026-10-10 (`ffffffff`), default mode 2026-10-20 (`aaaaaaaa`)")
        self.dm = fast_manifest("2026-10-10", "f" * 40, bench="c" * 64)
        self.assertNotIn("same protocol", self.figures()["fast_lead"])

    def test_wrapped_lead_is_a_block_and_stays_stable_under_generation(self):
        lead = self.figures()["fast_lead"]
        self.assertTrue(all(len(line) <= 80 for line in lead.splitlines()))


class HeadlineStatsTests(unittest.TestCase):
    def test_all_collections_and_setup_failures_participate_with_raw_parity(self):
        rows = [{"model": "pdb", "paired_speedup": "0.999"},
                {"model": "aalto_local", "paired_speedup": "3", "note": "build failed"},
                {"model": "brms_local", "paired_speedup": "1.4"},
                {"model": "rethinking_local", "paired_speedup": ""}]
        self.assertEqual(gen_docs.corpus_stats(rows), (3, 1.4, 2))

    def test_no_measured_gradients_is_not_a_zero_speedup(self):
        self.assertEqual(gen_docs.corpus_stats([{"paired_speedup": ""}]), (0, None, 0))


class BrowserBenchmarkTests(unittest.TestCase):
    def test_ratio_uses_paired_measurement_instead_of_ratio_of_medians(self):
        fields = benchmark_fields({"stanli_ns_grad": "100", "cmdstan_ns_grad": "200",
                                   "paired_speedup": "1.8", "params": "7", "stanli_sample_s": "4"})
        self.assertEqual(fields, {"speedup": 1.8, "us": .1, "params": 7})

    def test_missing_paired_ratio_does_not_fall_back_to_median_ratio(self):
        fields = benchmark_fields({"stanli_ns_grad": "100", "cmdstan_ns_grad": "200",
                                   "paired_speedup": "", "stanli_sample_s": ""})
        self.assertNotIn("speedup", fields)


if __name__ == "__main__":
    unittest.main()
