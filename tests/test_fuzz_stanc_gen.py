#!/usr/bin/env python3
"""Tests for tools/fuzz_stanc/gen.py, the random Stan program generator.

Nothing here runs stanc or stanli. The rule checker is tested on small
hand-built programs that break one rule each, and on generated programs,
which must break none.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import re
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))

import gen as g  # noqa: E402

SEEDS = range(300)


def program(**blocks):
    """A program with one size symbol of value 2 and the given blocks."""
    p = g.Program(seed=0, sizes=[2], config=g.config())
    p.datavars = [g.DataVar("N0", g.INT, "size", sym=0)]
    p.params = [g.Param("p0", g.REAL), g.Param("pv", g.vec(0))]
    for k, v in blocks.items():
        setattr(p, k, v)
    return p


def rules(p):
    return sorted({v.split(":")[0] for v in g.check_rules(p)})


P0 = g.Var("p0", g.REAL)
HALF = g.Lit(g.REAL, 0.5)


def rng():
    return g.Call("normal_rng", [g.Lit(g.REAL, 0.0), g.Lit(g.REAL, 1.0)],
                  g.REAL)


class DeterminismTest(unittest.TestCase):
    def test_same_seed_same_program(self):
        for seed in range(20):
            a, b = g.generate(seed), g.generate(seed)
            self.assertEqual(a.source(), b.source())
            self.assertEqual(a.data(), b.data())
            self.assertEqual(a.features(), b.features())

    def test_seeds_differ(self):
        self.assertGreater(len({g.generate(s).source() for s in range(20)}),
                           15)

    def test_hash_seed_does_not_matter(self):
        code = ("import sys, hashlib; sys.path.insert(0, %r); import gen; "
                "print(hashlib.sha256(''.join(gen.generate(s).source() "
                "for s in range(30)).encode()).hexdigest())"
                % str(REPO / "tools" / "fuzz_stanc"))
        digests = set()
        for hash_seed in ("1", "2"):
            env = dict(os.environ, PYTHONHASHSEED=hash_seed)
            out = subprocess.run([sys.executable, "-c", code], env=env,
                                 capture_output=True, text=True, check=True)
            digests.add(out.stdout.strip())
        here = hashlib.sha256("".join(
            g.generate(s).source() for s in range(30)).encode()).hexdigest()
        self.assertEqual(digests, {here})


class GeneratedProgramsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.programs = [g.generate(s) for s in SEEDS]

    def test_generated_programs_keep_the_rules(self):
        for p in self.programs:
            self.assertEqual(g.check_rules(p), [], f"seed {p.seed}")

    def test_sizes_and_empty_containers(self):
        sizes = [v for p in self.programs for v in p.sizes]
        self.assertEqual(set(sizes), {0, 1, 2, 3})
        self.assertGreater(sum(1 for p in self.programs if 0 in p.sizes),
                           len(self.programs) // 4)

    def test_every_feature_is_used(self):
        seen = set()
        for p in self.programs:
            seen.update(p.features())
        self.assertEqual(sorted(set(g.FEATURES) - seen), [])

    def test_tilde_feature_marks_every_tilde(self):
        for p in self.programs:
            self.assertEqual("s_tilde" in p.features(), "~" in p.source())

    def test_lupdf_feature_marks_every_target_with_lupdf(self):
        for p in self.programs:
            lupdf = any("target +=" in line and
                        re.search(r"_lup[dm]f\(", line)
                        for line in p.source().splitlines())
            self.assertEqual("s_lupdf" in p.features(), lupdf)

    def test_tuple_arguments_with_effects_are_common(self):
        n = sum("c_effect_in_tuple_arg" in p.features()
                for p in self.programs)
        self.assertGreater(n, len(self.programs) // 30)

    def test_call_sites_are_spread_through_blocks(self):
        last = sum(1 for p in self.programs
                   if p.gq and isinstance(p.gq[-1], g.Decl) and
                   isinstance(p.gq[-1].init, g.Call) and
                   p.gq[-1].init.fn is not None)
        self.assertLess(last, len(self.programs) // 4)

    def test_one_to_four_functions(self):
        names = [len({f.name for f in p.functions}) for p in self.programs]
        self.assertEqual(min(names), 1)
        self.assertEqual(max(names), 4)

    def test_data_matches_declarations(self):
        for p in self.programs[:50]:
            data = p.data()
            for d in p.datavars:
                self.assertIn(d.name, data)
            for s, v in enumerate(p.sizes):
                self.assertEqual(data[f"N{s}"], v)

    def test_a_weight_turns_a_feature_off(self):
        cfg = g.config({"weights": {"tuple": 0, "fn_recursive": 0}})
        for s in range(60):
            feats = g.generate(s, cfg).features()
            self.assertNotIn("t_tuple", feats)
            self.assertNotIn("fn_recursive", feats)

    def test_generation_is_fast(self):
        import time
        started = time.monotonic()
        for s in range(1000, 1100):
            g.generate(s).source()
        self.assertLess((time.monotonic() - started) / 100, 0.05)


class RuleTest(unittest.TestCase):
    """Each hand-built program breaks exactly one rule."""

    def test_clean_program(self):
        p = program(model=[g.Target(g.Call("tanh", [P0], g.REAL))])
        self.assertEqual(rules(p), [])

    def test_assigned_before_read(self):
        p = program(model=[g.Decl("x", g.REAL, [], None),
                           g.Target(g.Var("x", g.REAL))])
        self.assertEqual(rules(p), ["assigned"])
        p = program(model=[g.Decl("x", g.REAL, [], None),
                           g.Assign(g.Var("x", g.REAL), "=", HALF),
                           g.Target(g.Var("x", g.REAL))])
        self.assertEqual(rules(p), [])
        p = program(model=[g.Target(g.Var("nowhere", g.REAL))])
        self.assertEqual(rules(p), ["assigned"])

    def test_bounded_loops(self):
        seven = g.Lit(g.INT, 7)
        p = program(model=[g.For("i", seven, [g.Target(P0)])])
        self.assertEqual(rules(p), ["bounded"])
        p = program(model=[g.While("w", seven, [g.Target(P0)])])
        self.assertEqual(rules(p), ["bounded"])
        p = program(model=[g.For("i", g.Size(0, "data"), [g.Target(P0)])])
        self.assertEqual(rules(p), [])

    def test_bounded_recursion(self):
        n = g.Var("n", g.INT)
        base = g.If(g.Op("<=", n, g.Lit(g.INT, 0), g.INT),
                    [g.Return(HALF)], None, pinned=True)
        f = g.FunDef("r", "plain", [("n", g.INT)], g.REAL, [], recursive=True)
        down = g.Call("r", [g.Op("-", n, g.Lit(g.INT, 1), g.INT)], g.REAL,
                      fn=f)
        f.body = [base, g.Return(down)]
        call = g.Call("r", [g.Lit(g.INT, 3)], g.REAL, fn=f)
        p = program(functions=[f], model=[g.Target(call)])
        self.assertEqual(rules(p), [])
        deep = g.Call("r", [g.Lit(g.INT, 9)], g.REAL, fn=f)
        p = program(functions=[f], model=[g.Target(deep)])
        self.assertEqual(rules(p), ["bounded"])
        f.body = [g.Return(down)]
        p = program(functions=[f], model=[g.Target(call)])
        self.assertEqual(rules(p), ["bounded"])

    def test_finite_arithmetic(self):
        p = program(model=[g.Target(g.Call("exp", [P0], g.REAL))])
        self.assertEqual(rules(p), ["finite"])
        p = program(model=[g.Target(g.Call("log", [P0], g.REAL))])
        self.assertEqual(rules(p), ["finite"])
        p = program(model=[g.Target(g.Op("/", P0, P0, g.REAL))])
        self.assertEqual(rules(p), ["finite"])
        safe = g.Call("exp", [g.Call("tanh", [P0], g.REAL)], g.REAL)
        p = program(model=[g.Target(g.Div(P0, P0, g.REAL)),
                           g.Target(safe)])
        self.assertEqual(rules(p), [])

    def test_scales_stay_positive(self):
        def density(scale):
            return g.Target(g.Call("normal_lpdf", [P0, HALF, scale],
                                   g.REAL, bar=True))
        safe = g.Call("exp", [g.Call("tanh", [P0], g.REAL)], g.REAL)
        self.assertEqual(rules(program(model=[density(safe)])), [])
        self.assertEqual(rules(program(model=[density(HALF)])), [])
        self.assertEqual(rules(program(model=[density(P0)])), ["finite"])
        self.assertEqual(rules(program(model=[density(
            g.Lit(g.REAL, -0.5))])), ["finite"])
        self.assertEqual(rules(program(model=[g.Tilde(P0, "cauchy",
                                                      [HALF, P0])])),
                         ["finite"])
        rate = g.Call("poisson_log_rng", [P0], g.INT)
        self.assertEqual(rules(program(gq=[g.Decl("k", g.INT, [], rate)])),
                         ["finite"])

    def test_one_effect_per_statement(self):
        two = g.Op("+", rng(), rng(), g.REAL)
        p = program(gq=[g.Decl("y", g.REAL, [], two)])
        self.assertEqual(rules(p), ["one_effect"])
        p = program(gq=[g.Decl("y", g.REAL, [], rng()),
                        g.Decl("z", g.REAL, [], rng())])
        self.assertEqual(rules(p), [])
        printer = g.FunDef("pr", "plain", [("a", g.REAL)], g.REAL,
                           [g.Print(['"x"']), g.Return(g.Var("a", g.REAL))])
        both = g.Op("+", g.Call("pr", [P0], g.REAL, fn=printer),
                    g.Call("pr", [P0], g.REAL, fn=printer), g.REAL)
        p = program(functions=[printer], model=[g.Target(both)])
        self.assertEqual(rules(p), ["one_effect"])

    def test_indexes_in_range(self):
        pv = g.Var("pv", g.vec(0))
        p = program(model=[g.Target(g.Index(pv, [("one", g.Lit(g.INT, 3))],
                                            g.REAL))])
        self.assertEqual(rules(p), ["in_range"])
        p = program(model=[g.Target(g.Index(pv, [("one", g.Lit(g.INT, 2))],
                                            g.REAL))])
        self.assertEqual(rules(p), [])
        i = g.Var("i", g.INT)
        loop = g.For("i", g.Lit(g.INT, 3),
                     [g.Target(g.Index(pv, [("one", i)], g.REAL))])
        self.assertEqual(rules(program(model=[loop])), ["in_range"])
        loop.hi = g.Size(0, "data")
        self.assertEqual(rules(program(model=[loop])), [])
        p = program(model=[g.Target(g.Call("sum", [g.Index(
            pv, [("upto", g.Lit(g.INT, 3))], g.vec(0))], g.REAL))])
        self.assertEqual(rules(p), ["in_range"])
        p = program()
        p.sizes = [0]
        clamp = g.Clamp(g.Lit(g.INT, 1), g.Size(0, "data"))
        p.model = [g.Target(g.Index(pv, [("one", clamp)], g.REAL))]
        self.assertEqual(rules(p), ["in_range"])

    def test_multi_index_into_an_empty_base(self):
        p = program()
        p.sizes = [0, 1]
        p.datavars.append(g.DataVar("ii", g.arr(1, g.INT), "index",
                                    target=0))
        pv = g.Var("pv", g.vec(0))
        p.model = [g.Target(g.Call("sum", [g.Index(
            pv, [("multi", g.Var("ii", g.arr(1, g.INT)))], g.vec(1))],
            g.REAL))]
        self.assertEqual(rules(p), ["in_range"])
        p.sizes = [2, 1]
        self.assertEqual(rules(p), [])

    def test_effects_only_where_stan_allows(self):
        p = program(model=[g.Target(rng())])
        self.assertEqual(rules(p), ["block"])
        p = program(gq=[g.Target(HALF)])
        self.assertEqual(rules(p), ["block"])
        p = program(gq=[g.Decl("y", g.REAL, [], rng())])
        self.assertEqual(rules(p), [])

    def test_declared_sizes_match_the_type(self):
        def decl(size):
            return g.Decl("y", g.vec(0), [size], g.Lit(g.vec(0), 0.5))
        self.assertEqual(rules(program(gq=[decl(g.Lit(g.INT, 3))])),
                         ["in_range"])
        self.assertEqual(rules(program(gq=[decl(g.Lit(g.INT, 2))])), [])
        self.assertEqual(rules(program(gq=[decl(g.Size(0, "data"))])), [])

    def test_fixed_length_array_expression(self):
        arr = g.ArrE([HALF, HALF, HALF], g.arr(0, g.REAL))
        p = program(gq=[g.Decl("y", g.arr(0, g.REAL), [g.Size(0, "data")],
                               arr)])
        self.assertEqual(rules(p), ["in_range"])


class SourceTest(unittest.TestCase):
    def test_blocks_in_order(self):
        src = g.generate(3).source()
        order = [src.find(b) for b in (
            "functions {", "data {", "transformed data {", "parameters {",
            "transformed parameters {", "model {", "generated quantities {")]
        self.assertEqual(order, sorted(order))
        self.assertTrue(all(i >= 0 for i in order))

    def test_literals_follow_the_current_sizes(self):
        p = program()
        lit = g.Lit(g.vec(0), 0.5)
        self.assertEqual(g.expr_str(lit, p), "[0.50, 0.75]'")
        p.sizes = [0]
        self.assertEqual(g.expr_str(lit, p), "rep_vector(0.0, 0)")


if __name__ == "__main__":
    unittest.main()
