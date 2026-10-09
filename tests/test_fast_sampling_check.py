#!/usr/bin/env python3
"""The posterior-agreement metrics and flag rules of the fast-mode sampling check."""
import csv
import json
import math
import pathlib
import sys
import unittest

try:
    import numpy as np
    import scipy.stats  # noqa: F401
except ImportError:
    np = None

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "harnesses"))
FIXTURE = REPO / "tests" / "fixtures" / "sampling_check"

if np is not None:
    import fast_sampling_check as fsc


def fixture_draws():
    with open(FIXTURE / "draws.csv") as f:
        rows = list(csv.DictReader(f))
    names = [k for k in rows[0] if k not in ("chain", "iter")]
    chains = sorted({int(r["chain"]) for r in rows})
    out = {}
    for n in names:
        out[n] = np.array([[float(r[n]) for r in rows if int(r["chain"]) == c]
                           for c in chains])
    return out


def gaussian(seed, chains=4, n=1000, mean=0.0, sd=1.0):
    return np.random.default_rng(seed).normal(mean, sd, size=(chains, n))


def arm_from(columns, **extra):
    base = {
        "summary": {k: fsc.posterior_summary(v) for k, v in columns.items()},
        "draws": 4000,
        "divergent": 0,
        "max_treedepth": 0,
        "stay": 0,
        "gradients": 40000,
        "warmup_gradients": 50000,
        "n_params": len(columns),
    }
    base.update(extra)
    return base


@unittest.skipIf(np is None, "numpy and scipy are required")
class MetricTests(unittest.TestCase):
    def test_summary_matches_the_posterior_package(self):
        expected = json.loads((FIXTURE / "expected_posterior.json").read_text())
        for name, draws in fixture_draws().items():
            got = fsc.posterior_summary(draws)
            for key, value in expected[name].items():
                with self.subTest(column=name, metric=key):
                    self.assertAlmostEqual(got[key] / value, 1.0, places=6)

    def test_constant_column_has_no_diagnostics(self):
        got = fsc.posterior_summary(np.full((4, 100), 3.0))
        self.assertEqual(got["sd"], 0.0)
        self.assertTrue(math.isnan(got["rhat"]))
        self.assertTrue(math.isnan(got["ess_bulk"]))

    def test_discrete_column_with_constant_tail_indicator(self):
        rng = np.random.default_rng(3)
        x = np.where(rng.random((4, 1000)) < 0.99, 2.0, 1.0)
        got = fsc.posterior_summary(x)
        self.assertTrue(math.isfinite(got["mcse_mean"]))
        self.assertTrue(math.isnan(got["mcse_q05"]))
        y = np.where(rng.random((4, 1000)) < 0.99, 2.0, 1.0)
        result = fsc.compare_arms(arm_from({"k": x}), arm_from({"k": y}))
        self.assertEqual(result["n_columns"], 1)

    def test_z_critical_is_bonferroni(self):
        self.assertAlmostEqual(fsc.z_critical(1, 0.05), 1.959964, places=5)
        self.assertAlmostEqual(fsc.z_critical(10, 0.05), 2.807034, places=5)
        self.assertGreater(fsc.z_critical(1000, 0.01), fsc.z_critical(10, 0.01))

    def test_canonical_names(self):
        self.assertEqual(fsc.canonical_name("theta.3"), "theta[3]")
        self.assertEqual(fsc.canonical_name("z.2.10"), "z[2,10]")
        self.assertEqual(fsc.canonical_name("theta[3]"), "theta[3]")
        self.assertEqual(fsc.canonical_name("lp__"), "lp__")
        self.assertEqual(fsc.canonical_name("sigma"), "sigma")

    def test_stay_rate_counts_repeated_rows(self):
        x = np.array([[1.0, 1.0, 2.0, 2.0, 2.0, 3.0]])
        y = np.array([[5.0, 5.0, 5.0, 6.0, 6.0, 6.0]])
        self.assertEqual(fsc.stay_count([x, y]), 2)

    def test_parse_run_csv_splits_chains_warmup_and_counts_gradients(self):
        header = ("lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
                  "divergent__,energy__,mu,z.1\n")
        rows = []
        for chain in range(2):
            for it in range(5):
                lf = 3 if it < 2 else 7
                dv = 1 if (chain == 1 and it == 3) else 0
                td = 10 if (chain == 0 and it == 4) else 2
                rows.append(f"-1,0.9,0.1,{td},{lf},{dv},1,{chain + it},{it}\n")
        run = fsc.parse_run_csv(header + "".join(rows), chains=2, warmup=2,
                                samples=3)
        self.assertEqual(run["draws"], 6)
        self.assertEqual(run["warmup_gradients"], 12)
        self.assertEqual(run["gradients"], 42)
        self.assertEqual(run["divergent"], 1)
        self.assertEqual(run["max_treedepth"], 1)
        self.assertEqual(sorted(run["columns"]), ["mu", "z[1]"])
        self.assertEqual(run["columns"]["mu"].shape, (2, 3))
        self.assertEqual(run["columns"]["mu"][1].tolist(), [3.0, 4.0, 5.0])

    def test_parse_run_csv_rejects_a_short_run(self):
        header = ("lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
                  "divergent__,energy__,mu\n")
        with self.assertRaises(ValueError):
            fsc.parse_run_csv(header + "-1,0.9,0.1,2,3,0,1,0.5\n", chains=2,
                              warmup=1, samples=1)


@unittest.skipIf(np is None, "numpy and scipy are required")
class FlagTests(unittest.TestCase):
    def columns(self, seed, **kw):
        return {"a": gaussian(seed, **kw), "b": gaussian(seed + 1, **kw)}

    def test_exchangeable_arms_are_not_flagged(self):
        a = arm_from(self.columns(1))
        b = arm_from(self.columns(11))
        self.assertEqual(fsc.compare_arms(a, b)["flags"], [])

    def test_shifted_mean_is_flagged(self):
        cols = self.columns(1)
        shifted = self.columns(11)
        shifted["b"] = shifted["b"] + 0.3
        result = fsc.compare_arms(arm_from(cols), arm_from(shifted))
        self.assertIn("mean", [f["kind"] for f in result["flags"]])
        self.assertEqual(result["worst"]["mean"]["column"], "b")

    def test_wider_posterior_is_flagged_on_sd_only(self):
        cols = self.columns(1)
        wide = self.columns(11)
        wide["a"] = wide["a"] * 1.15
        kinds = [f["kind"] for f in
                 fsc.compare_arms(arm_from(cols), arm_from(wide))["flags"]]
        self.assertIn("sd", kinds)
        self.assertNotIn("mean", kinds)

    def test_rhat_above_threshold_in_one_arm_only(self):
        good = self.columns(1)
        bad = self.columns(11)
        shift = np.array([0.0, 0.0, 0.5, 0.5])[:, None]
        bad["a"] = bad["a"] + shift
        kinds = [f["kind"] for f in
                 fsc.compare_arms(arm_from(good), arm_from(bad))["flags"]]
        self.assertIn("rhat", kinds)
        both = [f["kind"] for f in
                fsc.compare_arms(arm_from(bad), arm_from(bad))["flags"]]
        self.assertNotIn("rhat", both)

    def test_divergence_rate_difference_is_flagged_only_when_material(self):
        cols = self.columns(1)
        other = self.columns(11)
        small = fsc.compare_arms(arm_from(cols, divergent=3),
                                 arm_from(other, divergent=0))
        self.assertNotIn("divergent", [f["kind"] for f in small["flags"]])
        large = fsc.compare_arms(arm_from(cols, divergent=120),
                                 arm_from(other, divergent=2))
        self.assertIn("divergent", [f["kind"] for f in large["flags"]])

    def test_treedepth_and_stay_rates_use_the_same_rule(self):
        cols = self.columns(1)
        other = self.columns(11)
        kinds = [f["kind"] for f in fsc.compare_arms(
            arm_from(cols, max_treedepth=400, stay=300),
            arm_from(other, max_treedepth=0, stay=0))["flags"]]
        self.assertIn("treedepth", kinds)
        self.assertIn("stay", kinds)

    def test_multiple_comparison_allowance_grows_with_columns(self):
        many = {f"x{i}": gaussian(100 + i) for i in range(200)}
        other = {f"x{i}": gaussian(500 + i) for i in range(200)}
        result = fsc.compare_arms(arm_from(many), arm_from(other))
        self.assertEqual(result["flags"], [])
        self.assertGreater(result["z_critical"], 3.5)

    def test_ess_per_gradient_ratio(self):
        cols = self.columns(1)
        a = arm_from(cols, gradients=40000)
        b = arm_from(cols, gradients=20000)
        result = fsc.compare_arms(a, b)
        self.assertAlmostEqual(result["ess_per_gradient_ratio"], 0.5, places=6)

    def test_columns_present_in_one_arm_are_compared_on_the_intersection(self):
        a = arm_from(self.columns(1))
        cols = self.columns(11)
        cols["c"] = gaussian(21)
        result = fsc.compare_arms(a, arm_from(cols))
        self.assertEqual(result["n_columns"], 2)


@unittest.skipIf(np is None, "numpy and scipy are required")
class PersistenceTests(unittest.TestCase):
    def comparison(self, flags):
        return {"flags": flags}

    def test_counts_seeds_and_direction_per_flag_kind(self):
        runs = [
            self.comparison([{"kind": "treedepth", "a": 0.2, "b": 0.5, "z": -9.0}]),
            self.comparison([{"kind": "treedepth", "a": 0.1, "b": 0.4, "z": -7.0}]),
            self.comparison([{"kind": "treedepth", "a": 0.6, "b": 0.2, "z": 8.0}]),
            self.comparison([]),
            self.comparison([{"kind": "mean", "column": "x", "z": 5.0}]),
        ]
        got = fsc.persistence(runs)
        self.assertEqual(got["treedepth"], {"flagged": 3, "of": 5, "same_direction": 2})
        self.assertEqual(got["mean"], {"flagged": 1, "of": 5, "same_direction": 1})
        self.assertNotIn("sd", got)

    def test_rhat_direction_is_which_arm_exceeds(self):
        runs = [self.comparison([{"kind": "rhat", "a": 1.02, "b": 1.003}]),
                self.comparison([{"kind": "rhat", "a": 1.004, "b": 1.03}])]
        self.assertEqual(fsc.persistence(runs)["rhat"],
                         {"flagged": 2, "of": 2, "same_direction": 1})


@unittest.skipIf(np is None, "numpy and scipy are required")
class ReferenceTests(unittest.TestCase):
    def test_reference_columns_are_canonical(self):
        draws = [{"theta[1]": [1.0, 2.0], "mu": [0.0, 1.0]},
                 {"theta[1]": [3.0, 4.0], "mu": [2.0, 3.0]}]
        cols = fsc.reference_columns(draws)
        self.assertEqual(sorted(cols), ["mu", "theta[1]"])
        self.assertEqual(cols["theta[1]"].shape, (2, 2))
        self.assertEqual(cols["theta[1]"][1].tolist(), [3.0, 4.0])

    def test_reference_arm_has_summary_only(self):
        ref = fsc.reference_arm({"a": gaussian(3, chains=10, n=1000)})
        self.assertIn("a", ref["summary"])
        result = fsc.compare_arms(arm_from({"a": gaussian(4)}), ref,
                                  compare_sampler=False)
        self.assertEqual(result["flags"], [])


if __name__ == "__main__":
    unittest.main()
