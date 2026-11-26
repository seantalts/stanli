#!/usr/bin/env python3
"""Tests for the ledger rules of tools/check_interp.py."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import check_interp as ci  # noqa: E402


class Ledger(unittest.TestCase):
    def test_agreement_needs_no_entry(self):
        for state in ci.AGREE:
            self.assertIsNone(ci.ledger_verdict(state, None))

    def test_a_new_difference_fails(self):
        self.assertIn("REGRESSED", ci.ledger_verdict("disagree", None))

    def test_an_entry_that_now_agrees_fails(self):
        entry = {"state": "solver_disagree", "cause": "solver tolerance"}
        self.assertIn("remove the ledger entry",
                      ci.ledger_verdict("agree_1e-10", entry))

    def test_a_changed_state_fails(self):
        entry = {"state": "gradient_only", "cause": "x"}
        self.assertIn("now disagree", ci.ledger_verdict("disagree", entry))
        self.assertIsNone(ci.ledger_verdict("gradient_only", entry))

    def test_untriaged_entries_fail(self):
        entry = {"state": "disagree", "cause": "UNTRIAGED: lp"}
        self.assertIn("no named cause", ci.ledger_verdict("disagree", entry))

    def test_entries_for_unknown_models_and_agreeing_states_fail(self):
        ledger = {"lit/gone": {"state": "disagree", "cause": "x"},
                  "lit/here": {"state": "agree_1e-10", "cause": "x"}}
        problems = ci.ledger_problems(ledger, ["lit/here"])
        self.assertEqual(len(problems), 2)

    def test_a_timeout_is_its_own_state(self):
        rec = {"stan": "x.stan", "compiled": {"state": "compile_fail"},
               "interp": {"state": "timeout"}, "cmp": {}}
        self.assertEqual(ci.classify(rec), "interp_timeout")


if __name__ == "__main__":
    unittest.main()
