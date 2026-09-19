#!/usr/bin/env python3
"""Corpus sampling must fail closed on incomplete or corrupt output."""
import pathlib
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from check_corpus_sampling import (parse_timings, reference_names, sample_csv,
                                   selected_cases, source_digest, SAMPLER_COLUMNS)


class CorpusSamplingTests(unittest.TestCase):
    def test_default_selects_metadata_without_model_or_collection_names(self):
        cases = {
            "unfamiliar_model": types.SimpleNamespace(metadata={"sampling_smoke": True}),
            "another": types.SimpleNamespace(metadata={}),
            "disabled": types.SimpleNamespace(metadata={"sampling_smoke": False}),
        }
        self.assertEqual(list(selected_cases(cases)), ["unfamiliar_model"])
        self.assertEqual(list(selected_cases(cases, ["another"])), ["another"])
        for names in ([], ["absent"], ["another", "another"]):
            with self.assertRaises(ValueError):
                selected_cases(cases, names)
        with self.assertRaises(ValueError):
            selected_cases({"another": cases["another"]})

    def test_reference_names_require_recorded_order_and_matching_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "model.stan"
            data = pathlib.Path(temporary) / "data.json"
            source.write_text("parameters {real theta;} model {theta ~ normal(0,1);}")
            data.write_text("{}")
            reference = {"source_sha256": source_digest(source), "data_sha256": source_digest(data),
                         "primary": 1, "points": {"1": {"wa": {"names": "theta,yrep"}}}}
            self.assertEqual(reference_names(reference, source, data), ["theta", "yrep"])
            for invalid in ({**reference, "points": {}}, {**reference, "primary": 0},
                            {**reference, "source_sha256": "bad"},
                            {**reference, "data_sha256": "bad"}):
                with self.assertRaises(ValueError):
                    reference_names(invalid, source, data)
            for invalid_names in (None, "", "theta,theta", "theta,", ["theta"]):
                invalid = {**reference, "points": {"1": {"wa": {"names": invalid_names}}}}
                with self.assertRaises(ValueError):
                    reference_names(invalid, source, data)

    def test_missing_or_invalid_phase_timings_fail(self):
        self.assertEqual(parse_timings("stanli_run: timings prep_s=1 sample_s=2 output_s=0"),
                         {"prep_s": 1, "sample_s": 2, "output_s": 0})
        for text in ("", "stanli_run: timings prep_s=nan sample_s=1 output_s=1",
                     "stanli_run: timings prep_s=-1 sample_s=1 output_s=1"):
            with self.assertRaises(ValueError):
                parse_timings(text)

    def test_csv_truncation_nonfinite_diagnostics_and_output_mismatch_fail(self):
        good = ("# comment\n" + ",".join(SAMPLER_COLUMNS + ["theta", "yrep"]) + "\n"
                "-1,0.9,0.1,3,7,0,2,0.2,4\n-2,0.8,0.1,3,7,0,3,0.3,5\n")
        self.assertEqual(len(sample_csv(good, ["theta", "yrep"], 2)), 2)
        for text in (good.rsplit("-2", 1)[0], good + "-3,0.9,0.1,3,7,0,4,0.4,6\n",
                     good.replace(",5", ",nan"),
                     good.replace("-2,", "inf,"), good.replace("yrep", "wrong"),
                     good.replace("theta,yrep", "yrep,theta"),
                     good.replace("theta,yrep", "theta,theta"),
                     good.replace("0.3,5", "0.3"), good.replace("0.3,5", "0.3,5,7"),
                     good.replace("0.3,5", "0.3,invalid")):
            with self.assertRaises(ValueError):
                sample_csv(text, ["theta", "yrep"], 2)
        # Complete finite draws still fail if any requested diagnostic is omitted.
        lines = good.splitlines()[1:]
        rows = [line.split(",") for line in lines]
        for index in range(len(SAMPLER_COLUMNS)):
            truncated = "\n".join(",".join(row[:index] + row[index+1:]) for row in rows)
            with self.subTest(diagnostic=SAMPLER_COLUMNS[index]):
                with self.assertRaisesRegex(ValueError, "seven standard"):
                    sample_csv(truncated, ["theta", "yrep"], 2)


if __name__ == "__main__":
    unittest.main()
