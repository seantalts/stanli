#!/usr/bin/env python3
"""Tests for tools/perturb/scale.py on small hand-written models and data.
Nothing here runs a compiler."""
from __future__ import annotations

import contextlib
import copy
import io
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "perturb"))

import scale  # noqa: E402


def decls(text):
    return {d.name: d for d in scale.parse_data_block(text)}


RADON = """
data {
  int<lower=0> N;
  int<lower=0> J;
  array[N] int<lower=1, upper=J> county;
  vector[N] x;
  vector[N] y;
  vector[J] u;
}
parameters {
  vector[J] alpha;
  real beta;
}
"""

RADON_DATA = {"N": 5, "J": 3, "county": [1, 2, 2, 3, 1],
              "x": [0.0, 1.0, 0.0, 1.0, 1.0], "y": [1.5, 2.5, 3.5, 4.5, 5.5],
              "u": [0.1, 0.2, 0.3]}

BRMS = """
data {
  int<lower=1> N;  // total number of observations
  array[N] int Y;
  int<lower=1> K;
  matrix[N, K] X;
  int<lower=1> N_1;
  int<lower=1> M_1;
  array[N] int<lower=1> J_1;
  vector[N] Z_1_1;
  int prior_only;
}
parameters {
  vector<lower=0>[M_1] sd_1;
  array[M_1] vector[N_1] z_1;
}
"""

BRMS_DATA = {"N": 4, "Y": [3, 0, 2, 7], "K": 2,
             "X": [[1, 0.5], [1, -0.5], [1, 1.5], [1, 2.0]],
             "N_1": 3, "M_1": 1, "J_1": [1, 3, 2, 3],
             "Z_1_1": [1.0, 1.0, 1.0, 1.0], "prior_only": 0}


class ParseTest(unittest.TestCase):
    def test_new_syntax_and_comments(self):
        d = decls("""
        functions { real f(real x) { return x; } }
        data {
          int<lower=0> N; /* a block
          comment with data { */
          array[N, 2] vector<lower=0>[K] v;  // trailing
          matrix<lower=fmin(0, 1), upper=[1, 2][1]>[N, K] X;
          real<offset=0, multiplier=2> s;
          cov_matrix[K] S;
          cholesky_factor_cov[M, K] L;
          cholesky_factor_cov[K] L2;
          simplex[K] theta;
          tuple(int, real) t;
        }
        transformed data { int M2 = N; }
        """)
        self.assertEqual(d["N"].basetype, "int")
        self.assertEqual(d["N"].lower, "0")
        self.assertEqual(d["N"].axes, [])
        self.assertEqual(d["v"].array_dims, ["N", "2"])
        self.assertEqual(d["v"].type_dims, ["K"])
        self.assertEqual(d["v"].axes, ["N", "2", "K"])
        self.assertEqual(d["X"].lower, "fmin(0, 1)")
        self.assertEqual(d["X"].upper, "[1, 2][1]")
        self.assertEqual(d["X"].axes, ["N", "K"])
        self.assertEqual(d["s"].axes, [])
        self.assertEqual(d["S"].axes, ["K", "K"])
        self.assertEqual(d["L"].axes, ["M", "K"])
        self.assertEqual(d["L2"].axes, ["K", "K"])
        self.assertEqual(d["theta"].axes, ["K"])
        self.assertTrue(d["t"].unsupported)
        self.assertNotIn("M2", d)

    def test_old_syntax_dims_lead(self):
        d = decls("data { int y[N]; real<lower=0> x[N, J]; vector[K] z[N]; }")
        self.assertEqual(d["y"].axes, ["N"])
        self.assertEqual(d["x"].axes, ["N", "J"])
        self.assertEqual(d["z"].array_dims, ["N"])
        self.assertEqual(d["z"].axes, ["N", "K"])

    def test_transformed_data_is_not_data(self):
        d = decls("transformed data { int A = 1; } data { int B; }")
        self.assertEqual(list(d), ["B"])

    def test_complex_has_pair_axis(self):
        d = decls("data { complex z; complex_vector[N] w; }")
        self.assertEqual(d["z"].axes, ["2"])
        self.assertEqual(d["w"].axes, ["N", "2"])

    def test_dims_elsewhere(self):
        dims = scale.parse_dims_elsewhere("""
        data { int N; int N_1; }
        parameters { array[M_1] vector[N_1] z; real a[P]; }
        transformed parameters { matrix<lower=0>[Q, R + 1] m; }
        """)
        for name in ("M_1", "N_1", "P", "Q", "R + 1"):
            self.assertIn(name, dims)
        self.assertNotIn("N", dims)


class SizeVariablesTest(unittest.TestCase):
    def test_radon_kinds(self):
        sv = scale.size_variables(RADON, RADON_DATA)
        self.assertEqual(sv["N"].kind, "rows")
        self.assertEqual(sv["J"].kind, "groups")
        self.assertFalse(sv["J"].inferred)
        self.assertIsNone(sv["N"].refused)
        self.assertEqual(sv["N"].lower, 0)
        self.assertEqual(sv["N"].value, 5)

    def test_brms_inferred_groups(self):
        sv = scale.size_variables(BRMS, BRMS_DATA)
        self.assertEqual(sv["N_1"].kind, "groups")
        self.assertTrue(sv["N_1"].inferred)
        self.assertEqual(sv["N"].kind, "rows")
        self.assertEqual(sv["K"].kind, "columns")
        self.assertNotIn("prior_only", sv)

    def test_parameter_only_variable_is_columns(self):
        sv = scale.size_variables(BRMS, BRMS_DATA)
        self.assertEqual(sv["M_1"].kind, "columns")

    def test_compound_dimension_refused(self):
        sv = scale.size_variables(
            "data { int N; vector[N + 1] y; vector[N] x; }",
            {"N": 2, "y": [1, 2, 3], "x": [1, 2]})
        self.assertIn("N + 1", sv["N"].refused)

    def test_compound_elsewhere_is_fine(self):
        sv = scale.size_variables(
            "data { int N; vector[N] x; } parameters { vector[N - 1] b; }",
            {"N": 2, "x": [1, 2]})
        self.assertIsNone(sv["N"].refused)

    def test_compound_only_use_refused(self):
        sv = scale.size_variables(
            "data { int N; int family; vector[N] y; } "
            "transformed data { vector[family == 3 ? N : 0] s; }",
            {"N": 2, "family": 1, "y": [1, 2]})
        self.assertTrue(sv["family"].refused.startswith("compound dimension"))
        self.assertIn("family == 3 ? N : 0", sv["family"].refused)
        self.assertIsNone(sv["N"].refused)

    def test_info_ignores_absent_size_variables(self):
        text = "data { int N; int M; vector[N] y; vector[M] z; }"
        _, info = scale.scale(text, {"N": 2, "y": [1, 2]}, "N", 3)
        self.assertEqual(info, {"N": 3})

    def test_scalar_bound_refused(self):
        sv = scale.size_variables(
            "data { int N; int<upper=N> k; vector[N] x; }",
            {"N": 2, "k": 1, "x": [1, 2]})
        self.assertIn("k", sv["N"].refused)

    def test_two_axes_refused(self):
        sv = scale.size_variables(
            "data { int N; matrix[N, N] D; }",
            {"N": 2, "D": [[0, 1], [1, 0]]})
        self.assertIn("D", sv["N"].refused)
        sv = scale.size_variables(
            "data { int K; cov_matrix[K] S; }",
            {"K": 2, "S": [[1, 0], [0, 1]]})
        self.assertIn("S", sv["K"].refused)

    def test_constrained_type_axis_refused(self):
        sv = scale.size_variables(
            "data { int N; int K; array[N] simplex[K] theta; }",
            {"N": 2, "K": 2, "theta": [[0.5, 0.5], [0.2, 0.8]]})
        self.assertIsNone(sv["N"].refused)
        self.assertIn("theta", sv["K"].refused)

    def test_ragged_refused(self):
        sv = scale.size_variables(
            "data { int N; int G; array[G] int s; vector[N] y; }",
            {"N": 5, "G": 2, "s": [2, 3], "y": [1, 2, 3, 4, 5]})
        self.assertEqual(sv["N"].refused,
                         "ragged group sizes s sum to N")

    def test_same_value_scalar_refused(self):
        sv = scale.size_variables(
            "data { int N; int n_obs; vector[N] y; }",
            {"N": 3, "n_obs": 3, "y": [1, 2, 3]})
        self.assertIn("n_obs", sv["N"].refused)

    def test_same_value_size_variable_is_fine(self):
        sv = scale.size_variables(
            "data { int N; int K; matrix[N, K] X; }",
            {"N": 2, "K": 2, "X": [[1, 2], [3, 4]]})
        self.assertIsNone(sv["N"].refused)
        self.assertIsNone(sv["K"].refused)

    def test_empty_axis_refused(self):
        sv = scale.size_variables("data { int<lower=0> N; vector[N] y; }",
                                  {"N": 0, "y": []})
        self.assertIn("empty", sv["N"].refused)

    def test_missing_variable_refused(self):
        sv = scale.size_variables("data { int N; vector[N] y; }", {"N": 2})
        self.assertIn("y", sv["N"].refused)

    def test_tuple_refused(self):
        sv = scale.size_variables(
            "data { int N; vector[N] y; tuple(array[N] int, real) t; }",
            {"N": 2, "y": [1, 2], "t": {"1": [1, 2], "2": 0.5}})
        self.assertIn("t", sv["N"].refused)

    def test_shape_mismatch_refused(self):
        sv = scale.size_variables("data { int N; vector[N] y; }",
                                  {"N": 3, "y": [1, 2]})
        self.assertIn("y", sv["N"].refused)

    def test_zero_size_container_written_empty(self):
        text = "data { int N; int<lower=0> P; array[N] vector[P] t; }"
        data = {"N": 3, "P": 0, "t": []}
        sv = scale.size_variables(text, data)
        self.assertIsNone(sv["N"].refused)
        new, _ = scale.scale(text, data, "N", 5)
        self.assertEqual(new["t"], [])

    def test_empty_container_with_expression_dimension(self):
        text = ("data { int S; array[3] int n; "
                "array[S] vector[max(n)] w; vector[S] v; }")
        data = {"S": 1, "n": [0, 0, 0], "w": [], "v": [1.0]}
        self.assertIsNone(scale.size_variables(text, data)["S"].refused)
        new, _ = scale.scale(text, data, "S", 2)
        self.assertEqual(new["w"], [])

    def test_non_numeric_bound_reason(self):
        sv = scale.size_variables("data { int n; int<lower=n> S; "
                                  "vector[S] v; }",
                                  {"n": 1, "S": 1, "v": [1.0]})
        self.assertTrue(sv["S"].refused.startswith("non-numeric bound"))

    def test_flags_of_zero_or_one_are_not_relations(self):
        text = "data { int K; int use_normal; vector[K] v; }"
        sv = scale.size_variables(text, {"K": 1, "use_normal": 1, "v": [1]})
        self.assertIsNone(sv["K"].refused)
        text = ("data { int<lower=0> P; int N; array[N] int z; vector[N] y;"
                " } parameters { vector[P] nu; }")
        sv = scale.size_variables(
            text, {"P": 0, "N": 2, "z": [0, 0], "y": [1, 2]})
        self.assertIsNone(sv["P"].refused)

    def test_inference_needs_unique_match(self):
        data = dict(BRMS_DATA, K=3, X=[[1, 0, 0]] * 4)
        sv = scale.size_variables(BRMS, data)
        self.assertNotEqual(sv["N_1"].kind, "groups")

    def test_inference_skips_own_dimension(self):
        sv = scale.size_variables(
            "data { int<lower=1> N; array[N] int<lower=1> y; }",
            {"N": 3, "y": [1, 3, 2]})
        self.assertEqual(sv["N"].kind, "rows")


class ScaleRowsTest(unittest.TestCase):
    def test_tile_and_truncate(self):
        orig = copy.deepcopy(RADON_DATA)
        new, info = scale.scale(RADON, RADON_DATA, "N", 7)
        self.assertEqual(RADON_DATA, orig)
        self.assertEqual(new["N"], 7)
        self.assertEqual(new["county"], [1, 2, 2, 3, 1, 1, 2])
        self.assertEqual(new["y"], [1.5, 2.5, 3.5, 4.5, 5.5, 1.5, 2.5])
        self.assertEqual(new["u"], RADON_DATA["u"])
        self.assertEqual(info, {"N": 7})
        new, _ = scale.scale(RADON, RADON_DATA, "N", 2)
        self.assertEqual(new["x"], [0.0, 1.0])

    def test_zero_respects_lower_bound(self):
        new, _ = scale.scale(RADON, RADON_DATA, "N", 0)
        self.assertEqual(new["y"], [])
        with self.assertRaises(scale.Refused):
            scale.scale(BRMS, BRMS_DATA, "N", 0)

    def test_rows_then_columns_on_matrix(self):
        new, info = scale.scale_many(BRMS, BRMS_DATA, {"N": 6, "K": 3})
        self.assertEqual(len(new["X"]), 6)
        self.assertTrue(all(len(r) == 3 for r in new["X"]))
        self.assertEqual(new["X"][4], [1, 0.5, 1])
        self.assertEqual(new["X"][0], [1, 0.5, 1])
        self.assertEqual(info, {"N": 6, "K": 3})

    def test_zero_columns(self):
        new, _ = scale.scale("data { int N; int<lower=0> K; matrix[N, K] X; }",
                             {"N": 2, "K": 1, "X": [[1], [2]]}, "K", 0)
        self.assertEqual(new["X"], [[], []])

    def test_nested_array_axis(self):
        text = "data { int N; int T; array[N, T] real y; }"
        new, _ = scale.scale(text, {"N": 2, "T": 2, "y": [[1, 2], [3, 4]]},
                             "T", 3)
        self.assertEqual(new["y"], [[1, 2, 1], [3, 4, 3]])

    def test_refused_variable_raises(self):
        with self.assertRaisesRegex(scale.Refused, "D"):
            scale.scale("data { int N; matrix[N, N] D; }",
                        {"N": 2, "D": [[0, 1], [1, 0]]}, "N", 3)

    def test_upper_bound_on_target(self):
        text = "data { int<lower=1, upper=4> K; vector[K] v; }"
        with self.assertRaises(scale.Refused):
            scale.scale(text, {"K": 2, "v": [1, 2]}, "K", 5)


class ScaleGroupsTest(unittest.TestCase):
    def test_replicate(self):
        new, info = scale.scale(RADON, RADON_DATA, "J", 6)
        self.assertEqual(new["J"], 6)
        self.assertEqual(new["N"], 10)
        self.assertEqual(new["county"], [1, 2, 2, 3, 1, 4, 5, 5, 6, 4])
        self.assertEqual(new["y"], RADON_DATA["y"] * 2)
        self.assertEqual(new["u"], RADON_DATA["u"] * 2)
        self.assertEqual(info, {"J": 6, "N": 10})

    def test_replicate_then_shrink(self):
        new, info = scale.scale(RADON, RADON_DATA, "J", 4)
        self.assertEqual(new["J"], 4)
        self.assertEqual(new["county"], [1, 2, 2, 3, 1, 4, 4])
        self.assertEqual(new["N"], 7)
        self.assertEqual(new["y"], [1.5, 2.5, 3.5, 4.5, 5.5, 1.5, 5.5])
        self.assertEqual(new["u"], [0.1, 0.2, 0.3, 0.1])
        self.assertEqual(info, {"J": 4, "N": 7})

    def test_shrink_below(self):
        new, _ = scale.scale(RADON, RADON_DATA, "J", 1)
        self.assertEqual(new["county"], [1, 1])
        self.assertEqual(new["N"], 2)
        self.assertEqual(new["y"], [1.5, 5.5])

    def test_shrink_respects_row_lower_bound(self):
        text = RADON.replace("int<lower=0> N", "int<lower=3> N")
        with self.assertRaisesRegex(scale.Refused, "N"):
            scale.scale(text, RADON_DATA, "J", 1)

    def test_brms_inferred(self):
        new, info = scale.scale(BRMS, BRMS_DATA, "N_1", 6)
        self.assertEqual(new["J_1"], [1, 3, 2, 3, 4, 6, 5, 6])
        self.assertEqual(new["N"], 8)
        self.assertEqual(new["M_1"], 1)
        self.assertEqual(len(new["X"]), 8)
        self.assertEqual(info, {"N_1": 6, "N": 8})

    def test_nested_groups(self):
        text = """data {
          int N; int J; int S;
          array[N] int<lower=1, upper=J> county;
          array[J] int<lower=1, upper=S> state;
          vector[N] y;
        } parameters { vector[J] a; vector[S] b; }"""
        data = {"N": 3, "J": 2, "S": 1, "county": [1, 2, 2], "state": [1, 1],
                "y": [1.0, 2.0, 3.0]}
        new, info = scale.scale(text, data, "S", 2)
        self.assertEqual(new["state"], [1, 1, 2, 2])
        self.assertEqual(new["county"], [1, 2, 2, 3, 4, 4])
        self.assertEqual(info, {"S": 2, "J": 4, "N": 6})
        with self.assertRaisesRegex(scale.Refused, "shrink"):
            scale.scale(text, dict(data, S=2, state=[1, 2]), "S", 1)
        new, _ = scale.scale(text, data, "J", 3)
        self.assertEqual(new["state"], [1, 1, 1])
        self.assertEqual(new["county"], [1, 2, 2, 3])

    def test_two_closure_axes_refused(self):
        text = """data { int N; int J; array[N] int<upper=J> g;
                  matrix[N, J] Z; }"""
        data = {"N": 2, "J": 2, "g": [1, 2], "Z": [[1, 0], [0, 1]]}
        with self.assertRaisesRegex(scale.Refused, "Z"):
            scale.scale(text, data, "J", 4)

    def test_groups_then_rows(self):
        new, info = scale.scale_many(RADON, RADON_DATA, {"N": 12, "J": 6})
        self.assertEqual(new["J"], 6)
        self.assertEqual(new["N"], 12)
        self.assertEqual(new["county"][10:], [1, 2])
        self.assertEqual(info, {"J": 6, "N": 12})

    def test_overlapping_group_closures_conflict(self):
        text = """data {
          int N; int J; int S;
          array[N] int<lower=1, upper=J> county;
          array[J] int<lower=1, upper=S> state;
        }"""
        data = {"N": 2, "J": 2, "S": 1, "county": [1, 2], "state": [1, 1]}
        with self.assertRaisesRegex(scale.Refused, "conflict"):
            scale.scale_many(text, data, {"J": 4, "S": 2})


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = scale.main([str(a) for a in args])
        return code, out.getvalue()

    def test_list_and_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "m.stan").write_text(RADON)
            (tmp / "d.json").write_text(json.dumps(RADON_DATA))
            code, out = self.run_cli(tmp / "m.stan", tmp / "d.json", "--list")
            self.assertEqual(code, 0)
            self.assertIn("J", out)
            self.assertIn("groups", out)
            code, out = self.run_cli(tmp / "m.stan", tmp / "d.json",
                                     "--set", "J=6", "--set", "N=20",
                                     "--out", tmp / "o.json")
            self.assertEqual(code, 0)
            written = json.loads((tmp / "o.json").read_text())
            self.assertEqual(written["N"], 20)
            self.assertIsInstance(written["county"][0], int)
            self.assertIn("N", out)


if __name__ == "__main__":
    unittest.main()
