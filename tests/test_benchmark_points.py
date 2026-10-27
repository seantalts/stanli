#!/usr/bin/env python3
"""Tests for the committed reference-run evaluation points and gradient counts."""

import gzip
import hashlib
import json
import os
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import benchmark_points as bp
import toolchain


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def document(models=None, warmup=10, draws=10):
    return dict(schema=bp.SCHEMA,
                settings=dict(seed=1, warmup=warmup, draws=draws, chains=1, points=2,
                              stanli_build_id="build"),
                models=models or {})


def reference(stan="s", data="d", vectors=((0.5, -1.0), (2.0, 3.0)), iterations=(3, 14)):
    return dict(source="reference_run", stan_sha256=digest(stan), data_sha256=digest(data),
                n_params=len(vectors[0]), points=[list(v) for v in vectors],
                iterations=list(iterations),
                gradients=dict(warmup=30, sampling=70, total=100),
                divergences=dict(warmup=1, sampling=0), max_treedepth=dict(warmup=0, sampling=2))


def fixed(stan="s", data="d", reason="reference run timed out after 60 s"):
    return dict(source="fixed", stan_sha256=digest(stan), data_sha256=digest(data), reason=reason)


class SelectionTests(unittest.TestCase):
    def test_systematic_selection_at_cumulative_leapfrog_quantiles(self):
        leapfrogs = [1] * 20
        self.assertEqual(bp.select_iterations(leapfrogs, 10), [0, 2, 4, 6, 8, 10, 12, 14, 16, 18])
        self.assertEqual(bp.select_iterations(leapfrogs, 4), [2, 7, 12, 17])

    def test_expensive_iterations_attract_more_points(self):
        leapfrogs = [1, 1, 1, 1, 100, 1, 1, 1, 1, 1]
        chosen = bp.select_iterations(leapfrogs, 10)
        self.assertEqual(len(chosen), 10)
        self.assertGreaterEqual(chosen.count(4), 8)

    def test_selection_is_nondecreasing_and_deterministic(self):
        leapfrogs = [3, 7, 1, 15, 31, 7, 3, 1, 1, 63, 7, 7]
        chosen = bp.select_iterations(leapfrogs, 5)
        self.assertEqual(chosen, sorted(chosen))
        self.assertEqual(chosen, bp.select_iterations(list(leapfrogs), 5))

    def test_selection_uses_exact_arithmetic_on_a_quantile_boundary(self):
        self.assertEqual(bp.select_iterations([5, 5], 1), [0])
        self.assertEqual(bp.select_iterations([5, 5, 5, 5], 2), [0, 2])

    def test_empty_or_zero_work_is_an_error(self):
        with self.assertRaises(ValueError):
            bp.select_iterations([], 10)
        with self.assertRaises(ValueError):
            bp.select_iterations([0, 0], 10)


class FileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name) / "points.json.gz"

    def write(self, doc):
        bp.write(self.path, doc)
        return doc

    def test_write_is_byte_deterministic_and_round_trips_doubles(self):
        doc = self.write(document({"m": reference(vectors=((0.1 + 0.2, 1e-310), (-2.5e300, 7.0)))}))
        first = self.path.read_bytes()
        self.write(doc)
        self.assertEqual(self.path.read_bytes(), first)
        self.assertEqual(first[4:8], b"\0\0\0\0")
        self.assertEqual(bp.load(self.path)["models"]["m"]["points"][0], [0.1 + 0.2, 1e-310])

    def test_entry_for_checks_both_hashes(self):
        self.write(document({"m": reference(), "f": fixed()}))
        doc = bp.load(self.path)
        self.assertEqual(bp.entry_for(doc, "m", digest("s"), digest("d"))["source"], "reference_run")
        self.assertEqual(bp.entry_for(doc, "f", digest("s"), digest("d"))["source"], "fixed")
        with self.assertRaisesRegex(ValueError, "stan source changed"):
            bp.entry_for(doc, "m", digest("other"), digest("d"))
        with self.assertRaisesRegex(ValueError, "data changed"):
            bp.entry_for(doc, "f", digest("s"), digest("other"))

    def test_a_model_without_an_entry_is_an_error(self):
        self.write(document())
        with self.assertRaisesRegex(ValueError, "no entry"):
            bp.entry_for(bp.load(self.path), "m", digest("s"), digest("d"))

    def test_points_and_counts_come_from_reference_entries_only(self):
        self.assertEqual(bp.points_of(reference()), [[0.5, -1.0], [2.0, 3.0]])
        self.assertIsNone(bp.points_of(fixed()))
        self.assertEqual(bp.gradient_count(reference()), 100)
        self.assertEqual(bp.gradient_count(fixed()), bp.DEFAULT_GRADIENTS)
        self.assertEqual(bp.DEFAULT_GRADIENTS, 5000)

    def test_a_fixed_entry_keeps_run_statistics_but_not_their_count(self):
        entry = dict(fixed(reason="draw 4 has a non-finite density"),
                     gradients=dict(warmup=1, sampling=2, total=3),
                     divergences=dict(warmup=0, sampling=0), max_treedepth=dict(warmup=0, sampling=0))
        self.write(document({"f": entry}))
        self.assertEqual(bp.gradient_count(bp.load(self.path)["models"]["f"]), bp.DEFAULT_GRADIENTS)

    def test_malformed_files_are_rejected(self):
        def with_(entry, **changes):
            return dict(entry, **changes)
        bad = {
            "schema": dict(document(), schema=99),
            "source": document({"m": with_(reference(), source="prior")}),
            "n_params": document({"m": with_(reference(), n_params=3)}),
            "ragged": document({"m": with_(reference(), points=[[1.0, 2.0], [3.0]])}),
            "count": document({"m": with_(reference(), points=[[1.0, 2.0]])}),
            "infinite": document({"m": with_(reference(), points=[[1.0, float("inf")], [1.0, 2.0]])}),
            "hash": document({"m": with_(reference(), stan_sha256="xyz")}),
            "iterations": document({"m": with_(reference(), iterations=[3])}),
            "iteration range": document({"m": with_(reference(), iterations=[3, 20])}),
            "iteration order": document({"m": with_(reference(), iterations=[14, 3])}),
            "total": document({"m": with_(reference(), gradients=dict(warmup=30, sampling=70, total=99))}),
            "negative": document({"m": with_(reference(), gradients=dict(warmup=-1, sampling=101, total=100))}),
            "no gradients": document({"m": {k: v for k, v in reference().items() if k != "gradients"}}),
            "no reason": document({"m": {k: v for k, v in fixed().items() if k != "reason"}}),
            "fixed with points": document({"m": with_(fixed(), points=[[1.0, 2.0]] * 2)}),
        }
        for label, doc in bad.items():
            with self.subTest(label=label):
                with gzip.GzipFile(self.path, "wb", mtime=0) as stream:
                    stream.write(json.dumps(doc).encode())
                with self.assertRaises(ValueError):
                    bp.load(self.path)

    def test_the_committed_file_covers_every_corpus_model(self):
        if not bp.DEFAULT_PATH.exists():
            self.skipTest("no committed points file")
        doc = bp.load(bp.DEFAULT_PATH)
        from corpus_inventory import corpus_cases, materialize_data
        cases = corpus_cases(REPO / "deps/posteriordb", "all")
        self.assertEqual(set(doc["models"]), set(cases))
        with tempfile.TemporaryDirectory() as scratch:
            for name, case in cases.items():
                bp.entry_for(doc, name, bp.sha256(case.source), bp.sha256(materialize_data(case, scratch)))


def stanli_library():
    names = ("libstanli.dylib", "libstanli.so")
    candidates = [pathlib.Path(os.environ["STANLI_LIB"])] if os.environ.get("STANLI_LIB") else [
        REPO / directory / name for directory in ("build-points", "build-rel") for name in names]
    return next((c for c in candidates if c.exists()), None)


PROBE = toolchain.vectorize_probe()
GOOD = """data { int<lower=0> N; vector[N] y; }
parameters { real mu; real<lower=0> sigma; }
model {
  mu ~ normal(0, 1);
  sigma ~ lognormal(0, 0.5);
  y ~ normal(mu, sigma);
}
"""
BROKEN = GOOD.replace("y ~ normal(mu, sigma);", "target += log(-square(mu) - 1);")
DATA = '{"N": 3, "y": [0.1, -0.2, 0.3]}'


@unittest.skipUnless(stanli_library() and PROBE.exists(), "needs a built libstanli and the vectorize probe")
class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.cases = {}
        for name, text in (("good", GOOD), ("broken", BROKEN)):
            stan, data = self.root / f"{name}.stan", self.root / f"{name}.json"
            stan.write_text(text)
            data.write_text(DATA)
            self.cases[name] = (stan, data)

    def generate(self, output, **changes):
        import gen_benchmark_points as gen
        options = dict(seed=7, warmup=30, draws=40, jobs=2, timeout=120)
        options.update(changes)
        gen.generate(self.cases, lib=stanli_library(), probe=PROBE, output=output, **options)
        return bp.load(output)

    def test_reference_run_records_ten_points_counts_and_hashes(self):
        doc = self.generate(self.root / "points.json.gz")
        self.assertEqual(sorted(doc["models"]), ["broken", "good"])
        good = doc["models"]["good"]
        self.assertEqual(good["source"], "reference_run")
        self.assertEqual((good["n_params"], len(good["points"]), len(good["iterations"])), (2, 10, 10))
        self.assertEqual(good["stan_sha256"], bp.sha256(self.cases["good"][0]))
        self.assertEqual(good["data_sha256"], bp.sha256(self.cases["good"][1]))
        gradients = good["gradients"]
        self.assertEqual(gradients["warmup"] + gradients["sampling"], gradients["total"])
        self.assertGreaterEqual(gradients["warmup"], 30)
        self.assertGreaterEqual(gradients["sampling"], 40)
        self.assertEqual(sorted(good["iterations"]), good["iterations"])
        self.assertTrue(all(0 <= i < 70 for i in good["iterations"]))
        self.assertEqual(doc["settings"]["points"], 10)
        self.assertEqual(doc["settings"]["warmup"], 30)

    def test_points_are_the_chosen_iterations_of_the_saved_warmup_run(self):
        import gen_benchmark_points as gen
        doc = self.generate(self.root / "points.json.gz")
        good = doc["models"]["good"]
        mir = self.root / "good.sexp"
        import subprocess
        subprocess.run([str(PROBE), "--vectorize-loops", "on", "--output", str(mir),
                        str(self.cases["good"][0])], check=True, capture_output=True)
        run = gen.run_chain(stanli_library(), mir, self.cases["good"][1], 7, 30, 40)
        self.assertEqual(good["iterations"], bp.select_iterations(run["n_leapfrog"], 10))
        n = good["n_params"]
        for point, index in zip(good["points"], good["iterations"]):
            self.assertEqual(point, run["draws"][index * n:(index + 1) * n])
        self.assertEqual(good["gradients"]["total"], int(sum(run["n_leapfrog"])))
        self.assertEqual(good["gradients"]["warmup"], int(sum(run["n_leapfrog"][:30])))

    def test_a_point_that_is_not_finite_on_the_data_falls_back_with_the_reason(self):
        doc = self.generate(self.root / "points.json.gz")
        broken = doc["models"]["broken"]
        self.assertEqual(broken["source"], "fixed")
        self.assertRegex(broken["reason"], r"reference run|non-finite|failed")

    def test_a_timeout_falls_back_and_is_recorded(self):
        doc = self.generate(self.root / "points.json.gz", warmup=1000000, draws=1000000, timeout=2)
        self.assertEqual(doc["models"]["good"]["source"], "fixed")
        self.assertIn("timed out after 2 s", doc["models"]["good"]["reason"])
        self.assertEqual(doc["settings"]["timeout_s"], 2)

    def test_rerun_is_byte_identical(self):
        self.generate(self.root / "a.json.gz")
        self.generate(self.root / "b.json.gz")
        self.assertEqual((self.root / "a.json.gz").read_bytes(), (self.root / "b.json.gz").read_bytes())


if __name__ == "__main__":
    unittest.main()
