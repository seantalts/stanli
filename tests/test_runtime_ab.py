#!/usr/bin/env python3
"""Pure accounting/parser tests for the paired runtime benchmark."""

import importlib.util
import csv
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "harnesses/runtime_ab.py"
SPEC = importlib.util.spec_from_file_location("runtime_ab", PATH)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)
sys.path.insert(0, str(PATH.parent))
from runtime_ab_export import portable_report, write_samples


class RuntimeABTests(unittest.TestCase):
    def test_transformed_data_print_is_not_result(self):
        self.assertEqual(BENCH.parse_bench("printed data\n123 2 80 7\n")["params"], 7)
        result = BENCH.compare_outputs("print\nOK 1 2\n", "OK 1 2\n")
        self.assertEqual(result["exact"], 2)

    def test_adjacent_values_and_zero_sign(self):
        adjacent = math.nextafter(1.0, math.inf)
        result = BENCH.compare_outputs("OK 1 -0\n", f"OK {adjacent!r} 0\n")
        self.assertEqual(result["max_ulp"], 1)
        self.assertEqual(result["different"], 2)
        self.assertEqual(result["zero_sign_changes"], 1)

    def test_negative_adjacent_values(self):
        adjacent = math.nextafter(-1.0, -math.inf)
        result = BENCH.compare_outputs("OK -1\n", f"OK {adjacent!r}\n")
        self.assertEqual(result["max_ulp"], 1)

    def test_no_infinity_or_nan_hides_change(self):
        result = BENCH.compare_outputs("OK inf nan\n", "OK -inf 1\n")
        self.assertEqual(result["nonfinite_changes"], 2)
        self.assertFalse(result["all_finite"])
        self.assertIsNone(BENCH.parse_bench("120 nan 50 1\n"))

    def test_failures_are_not_numerical_success(self):
        result = BENCH.compare_outputs("EVAL_FAIL bad\n", "EVAL_FAIL other\n")
        self.assertEqual(result["status"], "both_rejected")
        self.assertFalse(result["same_message"])
        self.assertEqual(BENCH.compare_outputs("OK 1", "EVAL_FAIL bad")["status"],
                         "status_changed")
        self.assertEqual(BENCH.compare_outputs("OK 1 2", "OK 1")["status"],
                         "length_changed")
        self.assertEqual(BENCH.compare_outputs("", "")["status"], "missing_output")

    def test_performance_parser_validates_output(self):
        self.assertEqual(BENCH.parse_bench("0.15 10", prep=True)["prep_s"], .15)
        for text in ("", "EVAL_FAIL", "nan 1 2 3", "0 1 2 3"):
            self.assertIsNone(BENCH.parse_bench(text))

    def test_environment_is_symmetric_and_explicit(self):
        with patch.dict("os.environ", {"STANLI_NO_SCAN": "1", "OMP_NUM_THREADS": "8"}):
            clean = BENCH.clean_environment()
            self.assertNotIn("STANLI_NO_SCAN", clean)
            self.assertEqual(clean["OMP_NUM_THREADS"], "1")
            explicit = BENCH.clean_environment({"STANLI_NO_SCAN": "1"})
            self.assertEqual(explicit["STANLI_NO_SCAN"], "1")

    def test_distribution_retains_dispersion(self):
        result = BENCH.distribution([1, 2, 3, 4, 100])
        self.assertEqual(result, {"median": 3, "q1": 2, "q3": 4, "min": 1, "max": 100})

    def test_shared_driver_keeps_build_specific_flags_and_libraries(self):
        output = ('clang++ -I/base/runtime/include -O3 -DNDEBUG -ffp-contract=off '
                  '-MD -MT object -MF object.d -o object -c /base/tools/bench_grad.cpp\n')
        with patch.object(BENCH.subprocess, "check_output", return_value=output):
            command = BENCH.shared_driver_command(Path("/base/build"), Path("/shared/bench_grad.cpp"))
        self.assertEqual(command[:5], ["clang++", "-I/base/runtime/include", "-O3", "-DNDEBUG", "-ffp-contract=off"])
        self.assertEqual(command[5], "/shared/bench_grad.cpp")
        self.assertIn("/base/build/libstanli.a", command)
        self.assertEqual(command[-2:], ["-o", "/base/build/bench_runtime_ab"])

    def test_shared_driver_rejects_unknown_command_shape(self):
        with patch.object(BENCH.subprocess, "check_output", return_value="ninja -v\n"):
            with self.assertRaises(RuntimeError):
                BENCH.shared_driver_command(Path("/base/build"), Path("/shared/bench_grad.cpp"))

    def test_export_keeps_measurements_but_not_local_inputs_or_commands(self):
        call = dict(status="ok", returncode=0, environment={}, peak_rss_mib=3,
                    command=["/private/binary"], measurement=dict(
                        prep_s=.2, gradient_ns=100, forward_ns=40))
        pair = dict(order=["before", "after"], before=dict(prep=call, gradient=call),
                    after=dict(prep=call, gradient=call))
        source = dict(metadata=dict(command=["/private/harness"], samples=1), results=[
            dict(name="fixture", group="test", status="measured", benchmark_point=2,
                 evaluations=100, inputs=dict(mir="/private/input", sha256={"mir": "hash"}),
                 correctness=[dict(status="compared", different=0,
                                   calls=dict(before=call, after=call))],
                 samples=[pair], profiles={})])
        portable = portable_report(source)
        self.assertNotIn("command", portable["metadata"])
        row = portable["results"][0]
        self.assertEqual(row["inputs"], {"mir": "hash"})
        self.assertNotIn("command", row["samples"][0]["before"]["gradient"])
        self.assertEqual(row["samples"][0]["before"]["gradient"]["measurement"], call["measurement"])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "samples.csv"
            write_samples(portable, output)
            self.assertNotIn(b"\r", output.read_bytes())
            with output.open() as stream:
                records = list(csv.DictReader(stream))
        self.assertEqual(len(records), 2)
        self.assertEqual(records[0]["gradient_ns"], "100")
        self.assertEqual(records[1]["point"], "2")
        self.assertEqual(records[1]["side"], "after")


if __name__ == "__main__":
    unittest.main()
