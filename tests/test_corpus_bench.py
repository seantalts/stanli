#!/usr/bin/env python3
"""Focused tests for the corpus benchmark TSV serializer."""

import csv
import io
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from harnesses.corpus_bench import (COLS, parse_grad_count, row_line,
    upgrade_header, Runner, PhaseFailure, parse_timing, check_pair,
    summarize_pairs, paired_order, open_run, validate_draws, summarize_sampling,
    sampling_order, sampling_limit)


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


if __name__ == "__main__":
    unittest.main()
