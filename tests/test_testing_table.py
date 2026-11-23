#!/usr/bin/env python3
"""The per-model tables that tools/gen_docs.py writes into TESTING.md."""
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import testing_table as table  # noqa: E402
import verify_refs as v  # noqa: E402


def entry(bound, closer):
    return {"bound": bound, "reason": "r", "closer": closer}


class CellTest(unittest.TestCase):
    def test_no_entry_is_the_default_bound(self):
        self.assertEqual(table._cell([]), str(v.DEFAULT_ULP_BOUND))

    def test_largest_bound_for_each_verdict_with_cmdstan_first(self):
        cell = table._cell([entry(16, "stanli"), entry(815, "stanli"),
                            entry(32, "cmdstan"), entry(12, "unknown")])
        self.assertEqual(cell, "32 (CmdStan closer); 12 (undecided); "
                               "815 (stanli closer)")

    def test_a_bound_over_a_million_is_in_scientific_notation(self):
        self.assertEqual(table._cell([entry(2637852908725, "unknown")]),
                         "2.6e12 (undecided)")


class CorpusRowsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = {row["model"]: row for row in table.corpus_rows()}
        cls.ledger = v.load_ulp_ledger()["platforms"][table.LEDGER_PLATFORM]["main"]

    def test_one_row_for_each_reference_model(self):
        self.assertEqual(set(self.rows), set(v.load_refs()[0]))

    def test_every_ledger_model_has_a_row(self):
        self.assertLessEqual(set(self.ledger), set(self.rows))

    def test_a_cell_shows_the_ledger_bound_of_its_kind(self):
        for model, entries in self.ledger.items():
            lp, grad, wa = self.rows[model]["cells"][2:5]
            for label, item in entries.items():
                cell = {"lp": lp, "grad": grad, "wa": wa}[v.output_kind(label)]
                worst = max(e["bound"] for name, e in entries.items()
                            if v.output_kind(name) == v.output_kind(label)
                            and e["closer"] == item["closer"])
                self.assertIn(f"{table._count(worst)} "
                              f"({table.CLOSER_TEXT[item['closer']]})", cell,
                              f"{model} {label}")

    def test_a_model_without_entries_shows_only_the_default(self):
        row = self.rows["arK"]
        self.assertEqual(row["cells"][2:5], ["10", "10", "10"])
        self.assertEqual(row["collection"], "posteriordb")

    def test_a_model_that_cmdstan_rejects_everywhere_has_no_limits(self):
        row = self.rows["s2_invgaussian"]
        self.assertEqual(row["cells"], [""] * 5)
        self.assertIn("rejects every point", row["note"])


if __name__ == "__main__":
    unittest.main()
