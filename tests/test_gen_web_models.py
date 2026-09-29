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
from gen_web_models import DRAWS_MIN, benchmark_fields


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
        (self.root / "docs").mkdir()
        (self.root / "docs/corpus-bench.tsv").write_text("model\tpaired_speedup\nold\t99\n")
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
        (self.root / "docs").mkdir()
        (self.root / "tests/educational/models").mkdir(parents=True)
        (self.root / "docs/verification.json").write_text(json.dumps({
            "pdb_case": {"status": "VERIFIED", "max_ulp": 0, "max_rel": 1e-13}}))
        (self.root / "docs/corpus-refs.json.gz").write_bytes(gzip.compress(json.dumps({
            "models": {"pdb_case": {"points": [{}, {}, {}]}}}).encode()))
        with mock.patch.object(gen_docs, "render_gradient_catalog", return_value="validated vector table") as vector:
            stats = gen_docs.compute()
        vector.assert_called_once_with()
        self.assertEqual(stats["benchmark_vectorized_catalog"], "validated vector table")
        self.assertEqual(stats["benchmark_models"], "2")
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
        self.assertEqual(fields, {"speedup": 1.8, "us": .1, "params": 7,
                                  "warmup": 100, "samples": 100})

    def test_draw_defaults_do_not_infer_sampling_duration_from_proxy(self):
        for estimate in (0.001, 10000):
            fields = benchmark_fields({"stanli_estimated_s": str(estimate)})
            self.assertEqual(fields["warmup"], DRAWS_MIN)
            self.assertEqual(fields["samples"], DRAWS_MIN)

    def test_missing_paired_ratio_does_not_fall_back_to_median_ratio(self):
        fields = benchmark_fields({"stanli_ns_grad": "100", "cmdstan_ns_grad": "200",
                                   "paired_speedup": "", "stanli_sample_s": ""})
        self.assertNotIn("speedup", fields)
        self.assertEqual(fields["samples"], DRAWS_MIN)


if __name__ == "__main__":
    unittest.main()
