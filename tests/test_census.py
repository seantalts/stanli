#!/usr/bin/env python3
"""Tests for the model census's coverage ratchet.

harnesses/model_census.py had no test of its own, and the thing it grew
here is a gate: it decides whether a run failed. What is tested is the
decision, not the census -- synthetic rows stand in for 1,231 models, so
nothing here needs stanc, CmdStan, a build, or the corpus.

The case that matters is #151's. #145 turned stanc3's
function-signatures/math/matrix/size.stan from `lowered`, and verified
bitwise over 2,742 values, into a COMPILE_FAIL, and the only oracle that
saw it had no memory to notice. The first test in RegressionTest is that
bug in miniature.
"""

from __future__ import annotations

import gzip
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))

from model_census import (BASELINE_SCHEMA, STATUSES,  # noqa: E402
                          _CENSUS_RUNG, compare_baseline, load_baseline,
                          write_baseline)

CORPUS = "deps/stanc3-src/test/integration/good"
SIZE = "function-signatures/math/matrix/size.stan"
TOOLS = {"check_sha256": "c" * 64,
         "stanc_sha256": "5" * 64, "stanc_version": "stanc3 ac69570 (Unix)",
         "corpus_head": "a" * 40, "cmdstan_head": "d" * 40}


def row(path, status="lowered", sha="1" * 64, data_sha="2" * 64,
        differential=None):
    return {"path": path, "status": status, "sha256": sha,
            "data_sha256": data_sha, "repro": f"build/stanli_check {path}",
            "differential": ({"status": differential} if differential
                             else None)}


def report(*rows, filter="", tools=None):
    return {"corpus": CORPUS, "filter": filter, "tools": dict(tools or TOOLS),
            "counts": {"crashed": 0, "timed_out": 0, "harness_error": 0},
            "rows": list(rows)}


def baseline_of(*rows, corpus=CORPUS, tools=None):
    """The baseline a run over `rows` would have recorded."""
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "baseline.json.gz"
        write_baseline(report(*rows, tools=tools), path)
        value = load_baseline(path)
    value["corpus"] = corpus
    return value


class RoundTripTest(unittest.TestCase):
    """What is written comes back, and comparing it to itself is green."""

    def test_a_recorded_run_compares_clean_against_itself(self):
        rows = [row("a.stan"), row("b.stan", "unsupported"),
                row(SIZE, differential="verified")]
        delta = compare_baseline(report(*rows), baseline_of(*rows))
        self.assertFalse(delta["blocked"])
        for key in ("regressed", "improved", "voided", "new_models",
                    "missing_models", "moved_tools"):
            self.assertEqual(delta[key], [], key)

    def test_the_gzipped_file_is_json_and_byte_stable(self):
        rows = [row("a.stan", differential="verified")]
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "baseline.json.gz"
            write_baseline(report(*rows), path)
            first = path.read_bytes()
            write_baseline(report(*rows), path)
            # mtime=0, so re-recording an unchanged census leaves the
            # checked-in file untouched and a diff means something moved.
            self.assertEqual(first, path.read_bytes())
        value = json.loads(gzip.decompress(first).decode("utf-8"))
        self.assertEqual(value["schema"], BASELINE_SCHEMA)
        self.assertEqual(value["models"]["a.stan"],
                         {"status": "lowered", "sha256": "1" * 64,
                          "data_sha256": "2" * 64,
                          "differential": "verified"})

    def test_an_unknown_schema_is_refused_not_half_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "old.json.gz"
            path.write_bytes(gzip.compress(
                json.dumps({"schema": 99, "models": {}}).encode()))
            with self.assertRaises(ValueError):
                load_baseline(path)

    def test_no_baseline_compares_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(load_baseline(pathlib.Path(tmp) / "absent.gz"))
        self.assertFalse(compare_baseline(report(row("a.stan")), None)
                         ["blocked"])


class LadderTest(unittest.TestCase):
    """The ordering itself, before any comparison uses it."""

    def test_every_census_status_sits_on_a_rung(self):
        # An unranked status is invisible to the ratchet: a model moving
        # into it would compare as "no verdict" and pass. The census's own
        # STATUSES tuple says the set is closed, so this holds it closed on
        # both sides -- adding a status without a rung has to fail here
        # rather than quietly open a hole.
        self.assertEqual(set(STATUSES), set(_CENSUS_RUNG))


class RegressionTest(unittest.TestCase):
    """Ground lost blocks, and says which model lost it."""

    def blocks(self, before, after):
        delta = compare_baseline(report(after), baseline_of(before))
        self.assertTrue(delta["blocked"])
        self.assertEqual(len(delta["regressed"]), 1)
        return delta["regressed"][0]

    def test_a_lowered_model_that_stops_lowering_blocks(self):
        # #151 exactly: the model still exists, its bytes and its data are
        # unchanged, and stanli stopped compiling it.
        moved = self.blocks(row(SIZE, differential="verified"),
                            row(SIZE, "unsupported", differential=None))
        self.assertEqual(moved["path"], SIZE)
        self.assertEqual((moved["from"], moved["to"]),
                         ("lowered", "unsupported"))
        self.assertIn(SIZE, moved["repro"])

    def test_a_lowered_model_that_starts_crashing_blocks(self):
        moved = self.blocks(row("a.stan"), row("a.stan", "crashed"))
        self.assertEqual(moved["to"], "crashed")

    def test_a_backlog_model_that_starts_crashing_blocks(self):
        # The census exits nonzero for the crash anyway; what the ratchet
        # adds is the name of the model that joined the two known dead ones.
        moved = self.blocks(row("a.stan", "unsupported"),
                            row("a.stan", "timed_out"))
        self.assertEqual((moved["from"], moved["to"]),
                         ("unsupported", "timed_out"))

    def test_a_model_that_stops_agreeing_with_cmdstan_blocks(self):
        moved = self.blocks(row("a.stan", differential="verified"),
                            row("a.stan", differential="mismatch"))
        self.assertEqual(moved["what"], "differential")
        self.assertEqual((moved["from"], moved["to"]),
                         ("verified", "mismatch"))

    def test_a_model_that_stops_comparing_any_values_blocks(self):
        # Still lowers, still agrees -- over nothing. rejected_both is
        # agreement with no compared value in it, so arriving there from
        # `verified` is thousands of numbers going unchecked.
        moved = self.blocks(row("a.stan", differential="verified"),
                            row("a.stan", differential="rejected_both"))
        self.assertEqual(moved["to"], "rejected_both")

    def test_agreed_refusal_turning_into_disagreement_blocks(self):
        moved = self.blocks(row("a.stan", differential="rejected_both"),
                            row("a.stan", differential="one_side_threw"))
        self.assertEqual(moved["to"], "one_side_threw")

    def test_a_model_vanishing_from_the_corpus_blocks(self):
        delta = compare_baseline(report(row("b.stan")),
                                 baseline_of(row("a.stan"), row("b.stan")))
        self.assertTrue(delta["blocked"])
        self.assertEqual(delta["missing_models"], ["a.stan"])

    def test_a_filtered_run_does_not_report_what_it_never_ran(self):
        delta = compare_baseline(report(row("b.stan"), filter="b"),
                                 baseline_of(row("a.stan"), row("b.stan")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["missing_models"], [])


class DirectionTest(unittest.TestCase):
    """Ground gained is recorded, never a failure."""

    def improves(self, before, after):
        delta = compare_baseline(report(after), baseline_of(before))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])
        self.assertEqual(len(delta["improved"]), 1)
        return delta["improved"][0]

    def test_a_refused_model_that_starts_lowering_passes(self):
        # Wiring up a feature is the point of the project. A gate that went
        # red for it would be re-baselined until nobody read it.
        moved = self.improves(row("a.stan", "unsupported"), row("a.stan"))
        self.assertEqual((moved["from"], moved["to"]),
                         ("unsupported", "lowered"))

    def test_a_lowered_model_that_starts_verifying_passes(self):
        moved = self.improves(row("a.stan", differential="one_side_threw"),
                              row("a.stan", differential="verified"))
        self.assertEqual(moved["to"], "verified")

    def test_a_lateral_move_inside_the_backlog_is_not_reported(self):
        # The line between these is the _DATA_REFUSAL regex. Gating it
        # would fire on tuning that heuristic, not on anything stanli did.
        delta = compare_baseline(report(row("a.stan", "data_rejected")),
                                 baseline_of(row("a.stan", "unsupported")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["improved"] + delta["regressed"], [])

    def test_a_new_model_is_recorded_not_gated(self):
        delta = compare_baseline(report(row("a.stan"), row("b.stan")),
                                 baseline_of(row("a.stan")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["new_models"], ["b.stan"])


class ShaTest(unittest.TestCase):
    """What the recorded verdict is conditioned on, and what voids it."""

    def test_a_changed_model_voids_its_row(self):
        # The stanc3-src pin advanced. The baseline describes a file that
        # no longer exists, so its verdict is not evidence of anything.
        delta = compare_baseline(
            report(row(SIZE, "unsupported", sha="9" * 64)),
            baseline_of(row(SIZE, differential="verified")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])
        self.assertEqual(delta["voided"], [f"{SIZE}: model changed"])

    def test_a_changed_model_voids_its_differential_too(self):
        delta = compare_baseline(
            report(row("a.stan", sha="9" * 64, differential="mismatch")),
            baseline_of(row("a.stan", differential="verified")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])

    def test_regenerated_data_still_gates_and_says_so(self):
        # The chosen policy, and the one edge worth pinning: voiding here
        # would disarm every row on any cold cache, which is every CI
        # runner. It blocks, and the note tells the reader the draw moved.
        delta = compare_baseline(
            report(row("a.stan", "eval_failed", data_sha="9" * 64)),
            baseline_of(row("a.stan")))
        self.assertTrue(delta["blocked"])
        self.assertIn("data regenerated", delta["regressed"][0]["note"])

    def test_regenerated_data_alone_is_not_a_regression(self):
        delta = compare_baseline(report(row("a.stan", data_sha="9" * 64)),
                                 baseline_of(row("a.stan")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])


class ToolchainTest(unittest.TestCase):
    """Which pins invalidate a comparison and which are only provenance."""

    def moved(self, **tools):
        return compare_baseline(report(row("a.stan"),
                                       tools=dict(TOOLS, **tools)),
                                baseline_of(row("a.stan")))

    def test_a_moved_stanc_pin_blocks(self):
        delta = self.moved(stanc_sha256="9" * 64)
        self.assertTrue(delta["blocked"])
        self.assertEqual(len(delta["moved_tools"]), 1)
        self.assertIn("stanc_sha256", delta["moved_tools"][0])

    def test_a_rebuilt_stanli_check_does_not_block(self):
        # The binary under test changes on every build that could matter.
        # Blocking on it would make the ratchet red exactly when something
        # happened and green when nothing did.
        delta = self.moved(check_sha256="9" * 64)
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["moved_tools"], [])

    def test_an_advanced_corpus_pin_does_not_block_wholesale(self):
        # It is recorded, and the models it actually changed void one at a
        # time on their own sha. A wholesale block would throw away every
        # still-valid row to protect the few that moved.
        delta = self.moved(corpus_head="9" * 40)
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["moved_tools"], [])

    def test_a_moved_cmdstan_pin_blocks(self):
        delta = self.moved(cmdstan_head="9" * 40)
        self.assertTrue(delta["blocked"])
        self.assertIn("cmdstan_head", delta["moved_tools"][0])

    def test_a_census_only_run_does_not_read_an_absent_cmdstan_as_moved(self):
        delta = self.moved(cmdstan_head=None)
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["moved_tools"], [])

    def test_a_different_corpus_voids_the_whole_comparison(self):
        delta = compare_baseline(report(row("a.stan")),
                                 baseline_of(row("a.stan"),
                                             corpus="some/other/tree"))
        self.assertFalse(delta["blocked"])
        self.assertEqual(len(delta["voided"]), 1)
        self.assertIn("whole baseline", delta["voided"][0])


class PhaseBTest(unittest.TestCase):
    """A run compares on what both sides measured, and nothing more."""

    def test_a_census_only_run_against_a_differential_baseline_is_green(self):
        # The likeliest way to get this wrong: 382 baseline rows carry a
        # `verified` verdict, a run without --differential carries none,
        # and reading that absence as ground lost would make every phase-A
        # run red.
        delta = compare_baseline(report(row("a.stan"), row("b.stan")),
                                 baseline_of(row("a.stan",
                                                 differential="verified"),
                                             row("b.stan",
                                                 differential="verified")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])

    def test_a_census_only_run_still_catches_the_151_regression(self):
        # The half that has to keep working without CmdStan: the ladder
        # that caught #151 is the census's own, so the cheap run catches it.
        delta = compare_baseline(report(row(SIZE, "unsupported")),
                                 baseline_of(row(SIZE,
                                                 differential="verified")))
        self.assertTrue(delta["blocked"])
        self.assertEqual(delta["regressed"][0]["path"], SIZE)

    def test_an_unbuildable_reference_is_no_verdict_not_a_regression(self):
        # ref_build_fail is a statement about CmdStan, not about stanli.
        delta = compare_baseline(
            report(row("a.stan", differential="ref_build_fail")),
            baseline_of(row("a.stan", differential="verified")))
        self.assertFalse(delta["blocked"])
        self.assertEqual(delta["regressed"], [])


class UpdateRefusalTest(unittest.TestCase):
    """What may not be recorded as the ground everyone else must hold."""

    def record(self, run, into=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "baseline.json.gz"
            if into is not None:
                write_baseline(into, path)
            write_baseline(run, path)
            return load_baseline(path)

    def test_a_partial_run_may_not_be_recorded(self):
        with self.assertRaises(ValueError) as caught:
            self.record(report(row("a.stan"), filter="tuples/"))
        self.assertIn("partial", str(caught.exception))

    def test_a_broken_harness_may_not_be_recorded(self):
        run = report(row("a.stan", "harness_error"))
        run["counts"]["harness_error"] = 1
        with self.assertRaises(ValueError) as caught:
            self.record(run)
        self.assertIn("harness_error", str(caught.exception))

    def test_a_row_with_no_status_may_not_be_recorded(self):
        with self.assertRaises(ValueError) as caught:
            self.record(report(row("a.stan", "invented")))
        self.assertIn("no status", str(caught.exception))

    def test_the_corpus_two_dead_models_may_be_recorded(self):
        # One model segfaults stanli and one does not terminate, both
        # permanently. Refusing them would mean no baseline could ever be
        # recorded; they belong on the bottom rung so a third joining them
        # is a named regression.
        run = report(row("a.stan", "crashed"), row("b.stan", "timed_out"))
        run["counts"].update(crashed=1, timed_out=1)
        self.assertEqual(len(self.record(run)["models"]), 2)

    def test_losing_phase_b_by_accident_is_refused(self):
        # Re-recording a differential baseline from a census-only run turns
        # 382 `verified` rows into `lowered` ones inside a gzipped file no
        # diff can show.
        with self.assertRaises(ValueError) as caught:
            self.record(report(row("a.stan")),
                        into=report(row("a.stan", differential="verified")))
        self.assertIn("--differential", str(caught.exception))


class CheckedInBaselineTest(unittest.TestCase):
    """The file this repository ships."""

    def test_it_loads_and_describes_the_pinned_corpus(self):
        from model_census import BASELINE, CORPUS, repo_rel
        value = load_baseline(BASELINE)
        self.assertIsNotNone(value)
        self.assertEqual(value["corpus"], repo_rel(CORPUS))
        # The recording ran phase B, so a later phase-B run has verdicts to
        # be held to. Losing that silently is what UpdateRefusalTest guards.
        self.assertTrue(any("differential" in m
                            for m in value["models"].values()))
        self.assertTrue(all(len(m["sha256"]) == 64
                            for m in value["models"].values()))


if __name__ == "__main__":
    unittest.main()
