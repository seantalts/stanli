#!/usr/bin/env python3
"""Tests for the ledger rules of tools/check_interp.py."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import check_interp as ci  # noqa: E402


class Ledger(unittest.TestCase):
    def test_agreement_needs_no_entry(self):
        self.assertIsNone(ci.ledger_verdict("agree_1e-10", None))

    def test_failing_the_same_way_needs_an_entry(self):
        for state in ("both_compile_fail_identical", "both_threw_everywhere"):
            self.assertIn("REGRESSED", ci.ledger_verdict(state, None))

    def test_a_new_difference_fails(self):
        self.assertIn("REGRESSED", ci.ledger_verdict("disagree", None))

    def test_an_entry_that_now_agrees_fails(self):
        entry = {"state": "solver_disagree", "cause": "solver tolerance"}
        self.assertIn("remove it from the ledger",
                      ci.ledger_verdict("agree_1e-10", entry))

    def test_a_changed_state_fails(self):
        entry = {"state": "gradient_only", "cause": "x"}
        self.assertIn("now disagree", ci.ledger_verdict("disagree", entry))
        self.assertIsNone(ci.ledger_verdict("gradient_only", entry))

    def test_untriaged_entries_fail(self):
        entry = {"state": "disagree", "cause": "UNTRIAGED: lp"}
        self.assertIn("no named cause", ci.ledger_verdict("disagree", entry))

    def test_bad_groups_fail(self):
        ledger = [{"state": "disagree", "cause": "x", "models": ["lit/gone"]},
                  {"state": "agree_1e-10", "cause": "x", "models": ["lit/a"]},
                  {"state": "lit_timeout", "cause": "x", "models": ["lit/b"]},
                  {"state": "disagree", "cause": "y",
                   "models": ["lit/c", "lit/c"]}]
        problems = ci.ledger_problems(ledger, ["lit/a", "lit/b", "lit/c"])
        self.assertEqual(len(problems), 4)

    def test_update_keeps_causes_and_skipped_models(self):
        ledger = [{"state": "skipped", "cause": "slow", "models": ["lit/s"]},
                  {"state": "disagree", "cause": "known", "models": ["lit/d"]}]
        recs = [{"corpus": "lit", "model": "d", "state": "disagree"},
                {"corpus": "lit", "model": "e", "state": "agree_1e-10"}]
        self.assertEqual(ci.update_ledger(ledger, recs), [
            {"state": "disagree", "cause": "known", "models": ["lit/d"]},
            {"state": "skipped", "cause": "slow", "models": ["lit/s"]}])

    def test_a_timeout_is_its_own_state(self):
        rec = {"stan": "x.stan", "compiled": {"state": "compile_fail"},
               "interp": {"state": "timeout"}, "cmp": {}}
        self.assertEqual(ci.classify(rec), "interp_timeout")


if __name__ == "__main__":
    unittest.main()
