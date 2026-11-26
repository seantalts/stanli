import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools" / "perturb"))
import datavary  # noqa: E402
import scale  # noqa: E402

RADON = """
data {
  int<lower=0> N;
  int<lower=0> J;
  array[N] int<lower=1, upper=J> county;
  matrix[N, 2] X;
  vector[N] y;
}
"""
DATA = {"N": 6, "J": 3, "county": [1, 2, 2, 3, 1, 3],
        "X": [[1.0, 0.5], [1.0, 1.5], [1.0, 2.5], [1.0, 3.5], [1.0, 4.5], [1.0, 5.5]],
        "y": [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]}


def analysis():
    return scale.Analysis(RADON, DATA)


class Perturbations(unittest.TestCase):
    def test_rows_axis_holds_every_row_container(self):
        self.assertEqual(datavary.row_axes(analysis()), {"N": ["county", "X", "y"]})

    def test_duplicates_copy_whole_rows(self):
        new = datavary.duplicate_rows(analysis(), "N", ["county", "X", "y"], 2)
        rows = list(zip(new["county"], map(tuple, new["X"]), new["y"]))
        self.assertEqual(len(set(rows)), 4)
        originals = set(zip(DATA["county"], map(tuple, DATA["X"]), DATA["y"]))
        self.assertTrue(set(rows) <= originals)

    def test_quantize_keeps_existing_values(self):
        col = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5]
        q = datavary.quantize(col, 2)
        self.assertEqual(set(q), {0.5, 5.5})
        self.assertEqual(len(set(datavary.quantize(col, 20))), 6)

    def test_constant_columns_are_not_perturbed(self):
        cols = [(n, j) for n, j, _ in datavary.real_columns(analysis(), ["X", "y"])]
        self.assertEqual(cols, [("X", 1), ("y", None)])

    def test_kinds(self):
        kinds = {k for k, *_ in datavary.perturbations(analysis())}
        self.assertEqual(kinds, {"duplicate_rows", "discrete_covariate",
                                 "constant_column", "zero_column",
                                 "index_order", "unbalanced_groups"})

    def test_unbalanced_groups(self):
        out = {label: d for k, label, _, d in datavary.perturbations(analysis())
               if k == "unbalanced_groups"}
        self.assertEqual(out["county 2 singletons"]["county"], [2, 3, 1, 1, 1, 1])

    def test_changed_needs_growth_or_a_new_lowering(self):
        base = {"ops": 21, "lowering": {}, "declines": []}
        ok = {"status": "ok"}
        self.assertIsNone(datavary.changed(base, {**ok, "ops": 25, "lowering": {}}))
        what, _ = datavary.changed(base, {**ok, "ops": 1118, "lowering": {},
                                          "declines": ["partition/log_prob: x"]})
        self.assertIn("21 -> 1118", what)
        what, _ = datavary.changed(base, {**ok, "ops": 21, "lowering": {"LOOP": 1}})
        self.assertIn("+lowering LOOP", what)
        self.assertIsNone(datavary.changed(base, {"status": "rejected"}))


class NewDeclines(unittest.TestCase):
    A = "reroll/log_prob: group is too small"

    def test_a_decline_the_base_already_has_counts_when_it_rises(self):
        base = {"ops": 20, "lowering": {}, "declines": [self.A],
                "decline_counts": {self.A: 2}}
        p = {"status": "ok", "ops": 90, "lowering": {}, "declines": [self.A],
             "decline_counts": {self.A: 9}}
        what, new = datavary.changed(base, p)
        self.assertEqual(new, [self.A])
        self.assertIn("ops 20 -> 90", what)


if __name__ == "__main__":
    unittest.main()
