#!/usr/bin/env python3
"""Tests for the pure parts of tools/hp: error metrics, validity and
comparison flags, direction sampling, and a few builtins and densities
against values known in closed form. No stanc, build or corpus is needed."""

from __future__ import annotations

import math
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "hp"))

import metrics  # noqa: E402

try:
    from mpmath import mp, mpf
    import mpdens
    import mpextra
    import mpfun
    import mptrans
    from mpvals import Mat, Vec
    HAVE_MP = True
except ImportError:
    HAVE_MP = False


class LpError(unittest.TestCase):
    def test_scaled_by_larger_magnitude_with_floor_of_one(self):
        self.assertEqual(metrics.lp_error(100.0, 101.0), 1.0 / 101.0)
        self.assertEqual(metrics.lp_error(0.25, 0.5), 0.25)

    def test_equal_and_matching_nonfinite_are_zero(self):
        self.assertEqual(metrics.lp_error(-math.inf, -math.inf), 0.0)
        self.assertEqual(metrics.lp_error(math.nan, math.nan), 0.0)

    def test_one_sided_nonfinite_is_infinite(self):
        self.assertEqual(metrics.lp_error(math.nan, 1.0), math.inf)
        self.assertEqual(metrics.lp_error(-math.inf, 3.0), math.inf)


class GradError(unittest.TestCase):
    def test_scaled_by_largest_entry_of_either_side(self):
        self.assertEqual(metrics.grad_error([1000.0, 0.0], [1000.0, 1e-3]), 1e-6)
        self.assertEqual(metrics.grad_error([0.0], [0.5]), 0.5)

    def test_floor_of_one(self):
        self.assertEqual(metrics.grad_error([1e-5], [2e-5]), 1e-5)

    def test_nonfinite_mismatch_is_infinite(self):
        self.assertEqual(metrics.grad_error([math.nan], [1.0]), math.inf)

    def test_empty_gradient_is_zero(self):
        self.assertEqual(metrics.grad_error([], []), 0.0)


class GateAgreement(unittest.TestCase):
    def test_metrics_match_verify_refs(self):
        import random
        sys.path.insert(0, str(REPO / "tools"))
        import verify_refs
        rng = random.Random(7)
        for _ in range(200):
            n = rng.randint(1, 8)
            a = [rng.uniform(-1e3, 1e3) * rng.choice([1, 1e-6]) for _ in range(n)]
            b = [x * (1 + rng.uniform(-1e-9, 1e-9)) for x in a]
            self.assertEqual(metrics.grad_error(a, b), verify_refs.fast_dev([0.0] + a, [0.0] + b)[0])
            x, y = rng.uniform(-1e4, 1e4), rng.uniform(-1e4, 1e4)
            self.assertEqual(metrics.lp_error(x, y), verify_refs.pair_dev(x, y)[0])


class Directions(unittest.TestCase):
    def test_unit_length_and_deterministic(self):
        a = metrics.direction("m:0:1", 50)
        b = metrics.direction("m:0:1", 50)
        self.assertEqual(a, b)
        self.assertAlmostEqual(math.sqrt(sum(x * x for x in a)), 1.0, places=14)

    def test_different_seeds_differ(self):
        self.assertNotEqual(metrics.direction("m:0:1", 5), metrics.direction("m:0:2", 5))

    def test_directional_error_uses_stated_scale(self):
        g = [3.0, 4.0]
        v = [0.6, 0.8]
        self.assertAlmostEqual(metrics.directional_error(g, v, 5.0, 5.0), 0.0, places=15)
        self.assertAlmostEqual(metrics.directional_error(g, v, 4.0, 5.0), 0.2, places=15)
        self.assertAlmostEqual(metrics.directional_error(g, v, 4.0, 0.5), 1.0, places=15)

    def test_scale_is_the_l2_norm(self):
        self.assertAlmostEqual(metrics.l2([3.0, 4.0]), 5.0, places=15)


class Validity(unittest.TestCase):
    def test_valid_when_both_arms_under_limit(self):
        self.assertEqual(metrics.validity([1e-14, 2e-13], [3e-14], ill=False), "valid")

    def test_over_limit_on_ill_conditioned_model(self):
        self.assertEqual(metrics.validity([2e-10], [2e-10], ill=True), "ill_conditioned")

    def test_over_limit_elsewhere_is_gate_failure(self):
        self.assertEqual(metrics.validity([2e-12], [1e-14], ill=False), "gate_fail")
        self.assertEqual(metrics.validity([1e-14], [2e-12], ill=False), "gate_fail")

    def test_limit_is_strict(self):
        self.assertEqual(metrics.validity([1e-12], [1e-12], ill=False), "gate_fail")

    def test_no_points_is_unscored(self):
        self.assertEqual(metrics.validity([], [], ill=False), "unscored")


class Compare(unittest.TestCase):
    def test_fast_worse_needs_ratio_and_floor(self):
        self.assertEqual(metrics.compare(1e-15, 1e-13), "fast_worse")
        self.assertEqual(metrics.compare(1e-16, 1e-15), "similar")
        self.assertEqual(metrics.compare(1e-13, 1.5e-13), "similar")

    def test_fast_better(self):
        self.assertEqual(metrics.compare(1e-13, 1e-15), "fast_better")
        self.assertEqual(metrics.compare(1e-15, 1e-16), "similar")

    def test_zero_default_error(self):
        self.assertEqual(metrics.compare(0.0, 5e-14), "fast_worse")
        self.assertEqual(metrics.compare(0.0, 5e-15), "similar")


class Percentiles(unittest.TestCase):
    def test_nearest_rank(self):
        xs = [float(i) for i in range(1, 101)]
        self.assertEqual(metrics.percentile(xs, 50), 50.0)
        self.assertEqual(metrics.percentile(xs, 100), 100.0)
        self.assertEqual(metrics.percentile(xs, 90), 90.0)

    def test_empty(self):
        self.assertTrue(math.isnan(metrics.percentile([], 50)))


@unittest.skipUnless(HAVE_MP, "mpmath not installed")
class Builtins(unittest.TestCase):
    def setUp(self):
        mp.dps = 50

    def close(self, a, b, tol=mpf(10) ** -45):
        self.assertLess(abs(a - b), tol * max(1, abs(b)))

    def test_student_t_lccdf_at_center_is_log_half(self):
        f = mpextra.CDFS["student_t_lccdf"]
        self.close(f(mpf(0), mpf(3), mpf(0), mpf(1)), -mp.log(2))

    def test_student_t_cdf_with_one_df_is_cauchy(self):
        f = mpextra.CDFS["student_t_cdf"]
        self.close(f(mpf(1), mpf(1), mpf(0), mpf(1)), mpf(3) / 4)

    def test_gamma_lcdf_exponential_case(self):
        f = mpextra.CDFS["gamma_lcdf"]
        self.close(f(mpf(2), mpf(1), mpf(1)), mp.log(1 - mp.exp(-2)))

    def test_poisson_lcdf(self):
        f = mpextra.CDFS["poisson_lcdf"]
        self.close(f(2, mpf(1)), mp.log(mp.exp(-1) * (1 + 1 + mpf(1) / 2)))

    def test_fma_on_vectors(self):
        r = mpfun.F["fma"](Vec([mpf(1), mpf(2)]), Vec([mpf(3), mpf(4)]), mpf(10))
        self.assertEqual([float(x) for x in r], [13.0, 18.0])

    def test_log2_without_argument_is_ln_two(self):
        self.close(mpfun.F["log2"](), mp.log(2))

    def test_append_col_of_two_vectors_is_a_matrix(self):
        r = mpfun.F["append_col"](Vec([mpf(1), mpf(2)]), Vec([mpf(3), mpf(4)]))
        self.assertIsInstance(r, Mat)
        self.assertEqual([[float(x) for x in row] for row in r], [[1.0, 3.0], [2.0, 4.0]])

    def test_matrix_exp_of_nilpotent(self):
        r = mpfun.F["matrix_exp"](Mat([[mpf(0), mpf(1)], [mpf(0), mpf(0)]]))
        self.assertEqual([[float(x) for x in row] for row in r], [[1.0, 1.0], [0.0, 1.0]])

    def test_eigenvalues_sym_ascending(self):
        r = mpfun.F["eigenvalues_sym"](Mat([[mpf(2), mpf(1)], [mpf(1), mpf(2)]]))
        self.assertEqual([float(x) for x in r], [1.0, 3.0])


@unittest.skipUnless(HAVE_MP, "mpmath not installed")
class Densities(unittest.TestCase):
    def setUp(self):
        mp.dps = 50

    def close(self, a, b, tol=mpf(10) ** -45):
        self.assertLess(abs(a - b), tol * max(1, abs(b)))

    def test_skew_normal_with_zero_shape_is_normal(self):
        args = [mpf("0.3"), mpf("-0.2"), mpf("1.7"), mpf(0)]
        a = mpextra.EXTRA_DENS["skew_normal"](args, [True] * 4, False)
        b = mpdens.lpdf("normal", args[:3], [True] * 3, False)
        self.close(a, b)

    def test_dirichlet_uniform_on_simplex(self):
        theta = Vec([mpf(1) / 3] * 3)
        alpha = Vec([mpf(1)] * 3)
        a = mpextra.EXTRA_DENS["dirichlet"]([theta, alpha], [True, False], False)
        self.close(a, mp.log(2))

    def test_dirichlet_propto_drops_constant_for_data_alpha(self):
        theta = Vec([mpf(1) / 3] * 3)
        alpha = Vec([mpf(2)] * 3)
        a = mpextra.EXTRA_DENS["dirichlet"]([theta, alpha], [True, False], True)
        self.close(a, 3 * mp.log(mpf(1) / 3))

    def test_lkj_corr_cholesky_uniform_two_by_two(self):
        L = Mat([[mpf(1), mpf(0)], [mpf("0.6"), mpf("0.8")]])
        a = mpextra.EXTRA_DENS["lkj_corr_cholesky"]([L, mpf(1)], [True, False], False)
        self.close(a, -mp.log(2) + mp.log(mpf("0.8")) * 0)

    def test_binomial_propto_with_data_probability_is_zero(self):
        a = mpdens.lpdf("binomial", [3, 10, mpf("0.4")], [False, False, False], True)
        self.assertEqual(a, 0)

    def test_lognormal_keeps_its_constant_under_propto(self):
        a = mpdens.lpdf("lognormal", [mpf(1), mpf(0), mpf(1)], [False, False, True], True)
        self.close(a, -mp.log(2 * mp.pi) / 2)

    def test_lognormal_with_all_data_arguments_is_dropped_under_propto(self):
        a = mpdens.lpdf("lognormal", [mpf(1), mpf(0), mpf(1)], [False, False, False], True)
        self.assertEqual(a, 0)

    def test_von_mises_zero_concentration_is_uniform(self):
        a = mpextra.EXTRA_DENS["von_mises"]([mpf("0.4"), mpf(0), mpf(0)], [True, True, True], False)
        self.close(a, -mp.log(2 * mp.pi))

    def test_multi_normal_identity_at_mean(self):
        S = Mat([[mpf(1), mpf(0)], [mpf(0), mpf(1)]])
        a = mpextra.EXTRA_DENS["multi_normal"]([Vec([mpf(0), mpf(0)]), Vec([mpf(0), mpf(0)]), S],
                                               [True, True, True], False)
        self.close(a, -mp.log(2 * mp.pi))


@unittest.skipUnless(HAVE_MP, "mpmath not installed")
class Transforms(unittest.TestCase):
    def setUp(self):
        mp.dps = 50

    def test_cholesky_corr_is_a_cholesky_factor_of_a_correlation_matrix(self):
        L, _ = mptrans.cholesky_corr([mpf("0.3"), mpf("-0.2"), mpf("0.5")], 3)
        for i in range(3):
            self.assertLess(abs(sum(L[i][k] ** 2 for k in range(3)) - 1), mpf(10) ** -45)

    def test_corr_matrix_has_unit_diagonal(self):
        C, _ = mptrans.corr_matrix([mpf("0.3"), mpf("-0.2"), mpf("0.5")], 3)
        for i in range(3):
            self.assertLess(abs(C[i][i] - 1), mpf(10) ** -45)

    def test_corr_matrix_of_zero_is_identity(self):
        C, _ = mptrans.corr_matrix([mpf(0)] * 3, 3)
        self.assertEqual([[float(x) for x in r] for r in C], [[1, 0, 0], [0, 1, 0], [0, 0, 1]])


class Report(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(REPO / "tools"))
        import report
        self.report = report

    def result(self, model, errs):
        points = {}
        for pt, (c, d, f) in enumerate(errs):
            points[str(pt)] = {"arms": {"cmdstan": {"lp": c, "grad": c},
                                        "default": {"lp": d, "grad": d},
                                        "fast": {"lp": f, "grad": f}}, "method": "fd"}
        return {"model": model, "n": 3, "points": points}

    def test_summary_takes_max_over_points(self):
        r = self.result("m", [(1e-16, 2e-16, 3e-16), (1e-15, 1e-15, 5e-14)])
        row = self.report.summarize({"m": r})["m"]
        self.assertEqual(row["fast", "lp"], 5e-14)
        self.assertEqual(row["default", "grad"], 1e-15)
        self.assertEqual(row["validity"], "valid")

    def test_excluded_point_is_not_scored(self):
        r = self.result("s2_com_poisson", [(1e-16, 1e-16, 1e-16), (0.9, 0.9, 0.9)])
        row = self.report.summarize({"s2_com_poisson": r})["s2_com_poisson"]
        self.assertEqual(row["default", "grad"], 1e-16)
        self.assertEqual(row["default", "lp"], 0.9)
        self.assertEqual(row["validity"], "gate_fail")

    def test_rejection_is_counted_not_scored(self):
        r = self.result("m", [(1e-16, 1e-16, 1e-16)])
        r["points"]["0"]["arms"]["fast"] = {"rejected": True}
        row = self.report.summarize({"m": r})["m"]
        self.assertEqual(row["fast", "rej"], 1)
        self.assertIsNone(row["fast", "lp"])
        self.assertEqual(row["rej_parity"], 1)

    def test_driver_names_the_restoring_switch(self):
        t = {"COLLAPSE": {"restores": True, "changes": True}, "CSE": {"restores": False, "changes": False}}
        self.assertEqual(self.report.drivers({"toggles": t}), "collapse")
        t["COLLAPSE"]["restores"] = False
        self.assertIn("collapse", self.report.drivers({"toggles": t}))


if __name__ == "__main__":
    unittest.main()
