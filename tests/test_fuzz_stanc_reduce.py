#!/usr/bin/env python3
"""Tests for tools/fuzz_stanc/reduce.py with fake "interesting" predicates,
so nothing here needs a compiler."""
from __future__ import annotations

import pathlib
import re
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))

import gen as g  # noqa: E402
import reduce as r  # noqa: E402


def lines(p):
    return len(p.source().splitlines())


class Recorder:
    """A predicate that records every candidate it sees."""

    def __init__(self, test):
        self.test = test
        self.seen = 0

    def __call__(self, p):
        self.seen += 1
        self.test.assertEqual(g.check_rules(p), [])
        return self.want(p)


class ReduceTest(unittest.TestCase):
    def test_always_interesting_shrinks_to_the_skeleton(self):
        rec = Recorder(self)
        rec.want = lambda p: True
        p = g.generate(7)
        small = r.reduce(p, rec)
        self.assertEqual(small.functions, [])
        for b in g.BLOCKS:
            self.assertEqual(getattr(small, b), [])
        self.assertEqual(small.params, [])
        self.assertEqual(set(small.sizes), {0})
        self.assertGreater(rec.seen, 10)
        self.assertEqual(g.check_rules(small), [])

    def test_keeps_what_the_predicate_needs(self):
        def has_rng_function_draw(p):
            return any(isinstance(e, g.Call) and e.fn is not None and
                       e.fn.kind == "rng" for e in g.all_exprs(p))
        seed = next(s for s in range(200)
                    if has_rng_function_draw(g.generate(s)))
        p = g.generate(seed)
        rec = Recorder(self)
        rec.want = has_rng_function_draw
        small = r.reduce(p, rec)
        self.assertTrue(has_rng_function_draw(small))
        self.assertLess(lines(small), lines(p))
        self.assertLessEqual(len(small.functions), 2)
        self.assertLess(lines(small), 25)

    def test_drops_a_function_argument_everywhere(self):
        def calls_with_one_arg(p):
            return any(isinstance(e, g.Call) and e.fn is not None and
                       len(e.fn.params) >= 1 for e in g.all_exprs(p))
        seed = next(s for s in range(200)
                    if any(len(f.params) >= 3 and not f.recursive and
                           f.kind == "plain"
                           for f in g.generate(s).functions))
        rec = Recorder(self)
        rec.want = calls_with_one_arg
        small = r.reduce(g.generate(seed), rec)
        self.assertTrue(all(len(f.params) <= 1 for f in small.functions))

    def test_does_not_change_the_input(self):
        p = g.generate(11)
        before = p.source()
        rec = Recorder(self)
        rec.want = lambda q: True
        r.reduce(p, rec)
        self.assertEqual(p.source(), before)

    def test_deterministic(self):
        def want(p):
            return "print" in p.source()
        a = r.reduce(g.generate(5), want)
        b = r.reduce(g.generate(5), want)
        self.assertEqual(a.source(), b.source())

    def test_parallel_batches_give_the_same_answer(self):
        def want(p):
            return "_rng(" in p.source()
        seed = next(s for s in range(100) if want(g.generate(s)))
        a = r.reduce(g.generate(seed), want, jobs=1)
        b = r.reduce(g.generate(seed), want, jobs=8)
        self.assertEqual(a.source(), b.source())


class EditTest(unittest.TestCase):
    def test_removing_a_variable_replaces_its_reads(self):
        p = next(q for q in map(g.generate, range(100))
                 if sum(isinstance(s, g.Decl) for s in q.gq) >= 2 and
                 not g.check_rules(q))
        name = next(s.name for s in p.gq if isinstance(s, g.Decl))
        q = r.remove_variable(p, name)
        self.assertIsNone(re.search(rf"\b{name}\b", q.source()))
        self.assertIsNotNone(re.search(rf"\b{name}\b", p.source()))
        self.assertEqual(g.check_rules(q), [])


if __name__ == "__main__":
    unittest.main()
