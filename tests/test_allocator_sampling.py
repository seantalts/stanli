#!/usr/bin/env python3
"""Adversarial checks for the allocator sampling report and frozen inputs."""
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/allocator"))
import sampling


class SamplingReportTest(unittest.TestCase):
    def records(self):
        return [dict(name="example", workers=4, round=rnd, slot=slot, result=dict(
            snapshot_sha256="complete-results", total_ns=value, sample_ns=value,
            prepare_ns=value, import_ns=value, peak_rss_bytes=value))
            for rnd in range(5) for slot, value in (
                ("system", 100), ("candidate", 80), ("system_aa", 100), ("candidate_aa", 80))]

    def test_ratios_and_controls(self):
        result = sampling.summarize(self.records(), [["example", 4]], 5)
        ratios = result["cells"][0]["ratios"]["total_ns"]
        self.assertEqual(result["processes"], 20)
        self.assertEqual(ratios["candidate"]["median"], 1.25)
        self.assertEqual(ratios["candidate"]["above_one"], 5)
        self.assertEqual(ratios["aa_system"]["median"], 1)
        self.assertEqual(ratios["aa_candidate"]["median"], 1)

    def test_missing_process_refused(self):
        with self.assertRaises(AssertionError):
            sampling.summarize(self.records()[:-1], [["example", 4]], 5)

    def test_changed_snapshot_refused(self):
        records = self.records()
        records[7]["result"]["snapshot_sha256"] = "changed-one-gradient-bit"
        with self.assertRaises(AssertionError):
            sampling.summarize(records, [["example", 4]], 5)

    def test_environment_has_no_allocator_preload(self):
        with mock.patch.dict(sampling.os.environ, {
                "LD_PRELOAD": "allocator.so", "STANLI_FORCE": "1",
                "OPENBLAS_NUM_THREADS": "4"}, clear=True):
            env = sampling.environment(pathlib.Path("/isolated/package"))
        self.assertNotIn("LD_PRELOAD", env)
        self.assertNotIn("STANLI_FORCE", env)
        self.assertEqual(env["OPENBLAS_NUM_THREADS"], "4")
        self.assertEqual(env["PYTHONPATH"], "/isolated/package")

    def test_package_and_input_hashes_are_frozen(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            case = root / "case"
            case.mkdir()
            (case / "model.stan").write_text("model {}")
            (case / "data.json").write_text("{}")
            for slot in ("system", "candidate"):
                (root / slot).mkdir()
                (root / slot / "runtime.so").write_bytes(b"frozen-library")
            inputs = root / "inputs.json"
            sampling.save(inputs, dict(cases=[dict(name="eight_schools_noncentered",
                input_dir=str(case), sha256={p.name: sampling.sha(p) for p in case.iterdir()})]))
            args = types.SimpleNamespace(inputs=inputs, system=root / "system",
                                         candidate=root / "candidate", smoke=True)
            fixed = sampling.manifest(args)
            self.assertEqual(fixed, sampling.manifest(args))
            (root / "candidate/runtime.so").write_bytes(b"changed-library")
            self.assertNotEqual(fixed, sampling.manifest(args))
            (case / "data.json").write_text('{"changed": true}')
            with self.assertRaises(AssertionError):
                sampling.manifest(args)


if __name__ == "__main__":
    unittest.main()
