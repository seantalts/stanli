#!/usr/bin/env python3
"""Tests for the statistics of tools/verify_posteriors.py, on synthetic draws."""

import math
import pathlib
import random
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import verify_posteriors as vp  # noqa: E402


def iid(seed, chains=4, n=1000, mean=0.0, sd=1.0):
    rng = random.Random(seed)
    return [[rng.gauss(mean, sd) for _ in range(n)] for _ in range(chains)]


def ar1(seed, phi, chains=4, n=4000):
    """Stationary AR(1) chains with unit marginal variance."""
    rng = random.Random(seed)
    scale = math.sqrt(1 - phi * phi)
    out = []
    for _ in range(chains):
        x, chain = rng.gauss(0, 1), []
        for _ in range(n):
            x = phi * x + scale * rng.gauss(0, 1)
            chain.append(x)
        out.append(chain)
    return out


class StatisticsTest(unittest.TestCase):

    def test_independent_draws_have_the_textbook_error(self):
        chains = iid(1)
        mean, se = vp.mcse_mean(chains)
        self.assertLess(abs(mean), 4 / math.sqrt(4000))
        self.assertAlmostEqual(se, 1 / math.sqrt(4000), delta=0.3 / math.sqrt(4000))
        self.assertAlmostEqual(vp.ess(chains) / 4000, 1.0, delta=0.4)
        sd, se_sd = vp.mcse_sd(chains)
        self.assertAlmostEqual(sd, 1.0, delta=0.05)
        # For a normal, se(sd) = sd / sqrt(2 N).
        self.assertAlmostEqual(se_sd, 1 / math.sqrt(8000), delta=0.4 / math.sqrt(8000))

    def test_autocorrelated_draws_have_a_smaller_ess(self):
        phi = 0.8
        expected = 4 * 4000 * (1 - phi) / (1 + phi)
        self.assertAlmostEqual(vp.ess(ar1(2, phi)) / expected, 1.0, delta=0.35)

    def test_rhat_is_one_for_mixed_chains_and_large_for_separated_ones(self):
        self.assertAlmostEqual(vp.split_rhat(iid(3)), 1.0, delta=0.01)
        chains = iid(4)
        chains[0] = [x + 3 for x in chains[0]]
        self.assertGreater(vp.split_rhat(chains), 1.1)

    def test_draws_from_the_same_posterior_pass(self):
        rows = vp.compare({"mu": iid(5), "tau": iid(6, sd=2)},
                          {"mu": iid(7, chains=10), "tau": iid(8, chains=10, sd=2)})
        self.assertEqual([r["name"] for r in rows], ["mu", "tau"])
        self.assertEqual(vp.verdict(rows), "PASS")
        for row in rows:
            self.assertLess(abs(row["z_mean"]), vp.Z_LIMIT)
            self.assertLess(abs(row["z_sd"]), vp.Z_LIMIT)

    def test_a_shifted_mean_or_a_wrong_scale_fails(self):
        ref = {"mu": iid(9, chains=10)}
        shifted = vp.compare({"mu": iid(10, mean=0.2)}, ref)
        self.assertEqual(vp.verdict(shifted), "FAIL")
        self.assertGreater(shifted[0]["z_mean"], vp.Z_LIMIT)
        wide = vp.compare({"mu": iid(11, sd=1.1)}, ref)
        self.assertEqual(vp.verdict(wide), "FAIL")
        self.assertGreater(wide[0]["z_sd"], vp.Z_LIMIT)

    def test_the_errors_of_both_sides_count(self):
        # With 25x fewer reference draws, the same shift is within noise.
        ref = {"mu": iid(12, chains=1, n=160)}
        rows = vp.compare({"mu": iid(13, mean=0.2)}, ref)
        self.assertLess(abs(rows[0]["z_mean"]), vp.Z_LIMIT)

    def test_a_constant_parameter(self):
        same = [[1.0] * 100 for _ in range(4)]
        rows = vp.compare({"c": same}, {"c": same})
        self.assertEqual(rows[0]["z_mean"], 0.0)
        self.assertEqual(vp.verdict(rows), "PASS")
        moved = vp.compare({"c": [[2.0] * 100 for _ in range(4)]}, {"c": same})
        self.assertEqual(vp.verdict(moved), "FAIL")
        self.assertEqual(vp.verdict([]), "NO_COMMON_PARAMETERS")


class ParsingTest(unittest.TestCase):

    def test_names_match_posteriordb(self):
        self.assertEqual(vp.canonical_name("theta.1.2"), "theta[1,2]")
        self.assertEqual(vp.canonical_name("mu"), "mu")
        self.assertEqual(vp.canonical_name("x.a"), "x.a")

    def test_rows_come_chain_by_chain(self):
        header = ("lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
                  "divergent__,energy__,theta.1")
        rows = [f"0,1,1,1,1,{int(c == 1 and i == 0)},0,{10 * c + i}"
                for c in range(2) for i in range(3)]
        draws, divergent = vp.parse_run("\n".join([header] + rows), 2, 3)
        self.assertEqual(draws["theta[1]"], [[0, 1, 2], [10, 11, 12]])
        self.assertEqual(divergent, 1)
        with self.assertRaises(ValueError):
            vp.parse_run("\n".join([header] + rows), 2, 4)


if __name__ == "__main__":
    unittest.main()
