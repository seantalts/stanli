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

    def test_the_limit_names_the_largest_bound_of_each_kind(self):
        for model, entries in self.ledger.items():
            limit = self.rows[model]["cells"][8]
            for kind in table.KINDS:
                bounds = [e["bound"] for label, e in entries.items()
                          if v.output_kind(label) == kind]
                if bounds:
                    self.assertIn(f"{table.KIND_NAME[kind]} "
                                  f"{table._count(max(bounds))}", limit, model)

    def test_a_model_without_entries_has_the_default_limit(self):
        row = self.rows["arK"]
        self.assertEqual(row["cells"][8], "10")
        self.assertEqual(row["collection"], "posteriordb")

    def test_measured_cells_are_the_recorded_means(self):
        record = table.load_accuracy()["models"]["arK"]
        cells = self.rows["arK"]["cells"]
        self.assertEqual(cells[2:4], [table._mean(record["lp"]["cmdstan"]["mean"]),
                                      table._mean(record["lp"]["stanli"]["mean"])])
        self.assertEqual(cells[4:6], [table._mean(record["grad"]["cmdstan"]["mean"]),
                                      table._mean(record["grad"]["stanli"]["mean"])])

    def test_an_unscored_kind_has_empty_cells(self):
        accuracy = table.load_accuracy()["models"]
        model = next(m for m, r in accuracy.items() if "unscored" in r["grad"]
                     and self.rows[m]["cells"][0])
        self.assertEqual(self.rows[model]["cells"][4:6], ["", ""])

    def test_accuracy_records_are_for_reference_models(self):
        self.assertLessEqual(set(table.load_accuracy()["models"]), set(self.rows))

    def test_a_model_that_cmdstan_rejects_everywhere_has_no_limits(self):
        row = self.rows["s2_invgaussian"]
        self.assertEqual(row["cells"], [""] * 9)
        self.assertIn("rejects every point", row["note"])


class TallyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REPO / "tools" / "hp"))
        import accuracy
        cls.accuracy = accuracy

    def record(self, *pairs):
        tally = self.accuracy.Tally()
        for i, (cmdstan, stanli) in enumerate(pairs):
            tally.add(f"grad[{i + 1}]", 0, cmdstan, stanli)
        return tally.record()

    def test_means_and_counts(self):
        record = self.record((2.0, 1.0), (4.0, 4.0), (0.0, 1.0))
        self.assertEqual(record["n"], 3)
        self.assertEqual(record["cmdstan"], {"mean": 2.0, "max": 4.0})
        self.assertEqual(record["stanli"], {"mean": 2.0, "max": 4.0})
        self.assertEqual((record["stanli_closer"], record["cmdstan_closer"]), (1, 1))
        self.assertNotIn("worse", record)

    def test_worse_is_over_ten_ulp_and_farther_than_cmdstan(self):
        record = self.record((3.0, 11.0), (12.0, 11.0), (3.0, 9.0), (11.0, 11.0))
        self.assertEqual(record["worse_count"], 1)
        self.assertEqual(record["worse"][0]["value"], "grad[1]")

    def test_a_value_that_both_engines_miss_by_millions_is_not_worse(self):
        record = self.record((6e7, 6e7 + 19))
        self.assertNotIn("worse", record)

    def test_no_values_is_no_record(self):
        self.assertIsNone(self.record())


class MeanTest(unittest.TestCase):
    def test_formats(self):
        self.assertEqual([table._mean(x) for x in
                          (0, 0.001, 0.071, 3.757, 16.89, 226.4, 566394.2)],
                         ["0", "<0.01", "0.07", "3.76", "16.9", "226", "5.7e5"])


if __name__ == "__main__":
    unittest.main()
