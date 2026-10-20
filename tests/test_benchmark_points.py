#!/usr/bin/env python3
"""Tests for the committed prior-draw evaluation points."""

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

BRMS = """data {
  int<lower=1> N;
  int prior_only;  // should the likelihood be ignored?
}
parameters { real b; }
model {
  b ~ normal(0, 1);
  if (!prior_only) { target += 1; }
}
"""


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def document(models=None, fallbacks=None):
    return dict(schema=bp.SCHEMA,
                settings=dict(seed=1, warmup=1000, draws=1000, chains=1,
                              selected_draws=[50, 150], stanli_build_id="build"),
                models=models or {}, fallbacks=fallbacks or {})


def entry(stan="s", data="d", vectors=((0.5, -1.0), (2.0, 3.0))):
    return dict(source="prior", stan_sha256=digest(stan), data_sha256=digest(data),
                n_params=len(vectors[0]), points=[list(v) for v in vectors])


class DetectionTests(unittest.TestCase):
    def test_data_declaration_marks_a_prior_only_model(self):
        self.assertTrue(bp.prior_only_model(BRMS))
        self.assertTrue(bp.prior_only_model(BRMS.replace(
            "int prior_only;", "int<lower=0, upper=1> prior_only;")))

    def test_other_mentions_do_not(self):
        self.assertFalse(bp.prior_only_model(
            "data { int N; }\nparameters { real b; }\nmodel { b ~ normal(0, 1); }\n"))
        self.assertFalse(bp.prior_only_model(
            "data { int N; }\n// int prior_only;\nmodel { }\n"))
        self.assertFalse(bp.prior_only_model(
            "data { int N; }\ntransformed data { int prior_only = 0; }\nmodel { }\n"))
        self.assertFalse(bp.prior_only_model(
            "data { int N; }\ntransformed data { int prior_only; prior_only = 0; }\nmodel { }\n"))
        self.assertFalse(bp.prior_only_model(
            "data { real prior_only; }\nmodel { }\n"))

    def test_inventory_selects_exactly_the_brms_style_models(self):
        from corpus_inventory import corpus_cases
        found = {name for name, case in corpus_cases(REPO / "deps/posteriordb", "all").items()
                 if bp.prior_only_model(case.source.read_text())}
        self.assertEqual(len(found), 149)
        self.assertIn("s2_com_poisson", found)
        self.assertNotIn("arK", found)


class FileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name) / "points.json.gz"

    def write(self, doc):
        bp.write(self.path, doc)
        return doc

    def test_write_is_byte_deterministic_and_round_trips_doubles(self):
        doc = self.write(document({"m": entry(vectors=((0.1 + 0.2, 1e-310), (-2.5e300, 7.0)))}))
        first = self.path.read_bytes()
        self.write(doc)
        self.assertEqual(self.path.read_bytes(), first)
        self.assertEqual(first[4:8], b"\0\0\0\0")
        loaded = bp.load(self.path)
        self.assertEqual(loaded["models"]["m"]["points"][0], [0.1 + 0.2, 1e-310])

    def test_points_for_returns_vectors_for_matching_hashes(self):
        doc = self.write(document({"m": entry()}))
        self.assertEqual(bp.points_for(bp.load(self.path), "m", digest("s"), digest("d")),
                         [[0.5, -1.0], [2.0, 3.0]])

    def test_hash_mismatch_is_an_error(self):
        self.write(document({"m": entry()}))
        doc = bp.load(self.path)
        with self.assertRaisesRegex(ValueError, "stan source changed"):
            bp.points_for(doc, "m", digest("other"), digest("d"))
        with self.assertRaisesRegex(ValueError, "data changed"):
            bp.points_for(doc, "m", digest("s"), digest("other"))

    def test_fallback_models_use_fixed_points_but_still_check_hashes(self):
        self.write(document(fallbacks={"m": dict(
            reason="draw 3 has a non-finite gradient", stan_sha256=digest("s"),
            data_sha256=digest("d"))}))
        doc = bp.load(self.path)
        self.assertIsNone(bp.points_for(doc, "m", digest("s"), digest("d")))
        with self.assertRaisesRegex(ValueError, "stan source changed"):
            bp.points_for(doc, "m", digest("other"), digest("d"))

    def test_unlisted_models_are_fixed_unless_they_look_like_prior_models(self):
        self.write(document())
        doc = bp.load(self.path)
        self.assertIsNone(bp.points_for(doc, "arK", digest("s"), digest("d"), BRMS.replace("int prior_only;", "int n;")))
        with self.assertRaisesRegex(ValueError, "no entry"):
            bp.points_for(doc, "m", digest("s"), digest("d"), BRMS)

    def test_malformed_files_are_rejected(self):
        bad = [
            dict(document(), schema=99),
            document({"m": dict(entry(), source="fixed")}),
            document({"m": dict(entry(), n_params=3)}),
            document({"m": dict(entry(), points=[[1.0, 2.0], [3.0]])}),
            document({"m": dict(entry(), points=[])}),
            document({"m": dict(entry(), points=[[1.0, float("inf")]])}),
            document({"m": dict(entry(), stan_sha256="xyz")}),
            document({"m": entry()}, {"m": dict(reason="x", stan_sha256=digest("s"),
                                                data_sha256=digest("d"))}),
        ]
        for index, doc in enumerate(bad):
            with self.subTest(index=index):
                with gzip.GzipFile(self.path, "wb", mtime=0) as stream:
                    stream.write(json.dumps(doc).encode())
                with self.assertRaises(ValueError):
                    bp.load(self.path)

    def test_the_committed_file_is_valid_and_matches_the_corpus(self):
        if not bp.DEFAULT_PATH.exists():
            self.skipTest("no committed points file")
        doc = bp.load(bp.DEFAULT_PATH)
        from corpus_inventory import corpus_cases, materialize_data
        cases = corpus_cases(REPO / "deps/posteriordb", "all")
        with tempfile.TemporaryDirectory() as scratch:
            for name, case in cases.items():
                if not bp.prior_only_model(case.source.read_text()):
                    continue
                data = materialize_data(case, scratch)
                bp.points_for(doc, name, bp.sha256(case.source), bp.sha256(data),
                              case.source.read_text())


def stanli_library():
    names = ("libstanli.dylib", "libstanli.so")
    candidates = [pathlib.Path(os.environ["STANLI_LIB"])] if os.environ.get("STANLI_LIB") else [
        REPO / directory / name for directory in ("build-points", "build-rel") for name in names]
    return next((c for c in candidates if c.exists()), None)


PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
PRIOR = """data { int<lower=0> N; vector[N] y; int prior_only; }
parameters { real mu; real<lower=0> sigma; }
model {
  mu ~ normal(0, 1);
  sigma ~ lognormal(0, 0.5);
  if (!prior_only) y ~ normal(mu, sigma);
}
"""
BROKEN = PRIOR.replace("y ~ normal(mu, sigma);", "target += log(-square(mu) - 1);")
PLAIN = "data { int N; }\nparameters { real mu; }\nmodel { mu ~ normal(0, 1); }\n"
DATA = '{"N": 3, "y": [0.1, -0.2, 0.3], "prior_only": 0}'


@unittest.skipUnless(stanli_library() and PROBE.exists(), "needs a built libstanli and the vectorize probe")
class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.cases = {}
        for name, text in (("good", PRIOR), ("broken", BROKEN), ("plain", PLAIN)):
            stan, data = self.root / f"{name}.stan", self.root / f"{name}.json"
            stan.write_text(text)
            data.write_text(DATA if name != "plain" else '{"N": 1}')
            self.cases[name] = (stan, data)

    def generate(self, output):
        import gen_benchmark_points as gen
        gen.generate(self.cases, lib=stanli_library(), probe=PROBE, output=output,
                     seed=7, warmup=30, draws=40, jobs=2, timeout=120)
        return bp.load(output)

    def test_prior_models_get_ten_draws_and_failures_fall_back(self):
        doc = self.generate(self.root / "points.json.gz")
        self.assertEqual(doc["settings"]["selected_draws"], [2, 6, 10, 14, 18, 22, 26, 30, 34, 38])
        self.assertEqual(sorted(doc["models"]), ["good"])
        good = doc["models"]["good"]
        self.assertEqual((good["n_params"], len(good["points"])), (2, 10))
        self.assertEqual(good["stan_sha256"], bp.sha256(self.cases["good"][0]))
        self.assertEqual(good["data_sha256"], bp.sha256(self.cases["good"][1]))
        self.assertEqual(sorted(doc["fallbacks"]), ["broken"])
        self.assertIn("non-finite", doc["fallbacks"]["broken"]["reason"])
        self.assertEqual(len({tuple(p) for p in good["points"]}), 10)

    def test_unbounded_prior_draws_fall_back(self):
        import gen_benchmark_points as gen
        output = self.root / "bounded.json.gz"
        doc = gen.generate({"good": self.cases["good"]}, lib=stanli_library(), probe=PROBE,
                           output=output, seed=7, warmup=30, draws=40, jobs=1, timeout=120,
                           max_abs=0.01)
        self.assertEqual(doc["models"], {})
        self.assertRegex(doc["fallbacks"]["good"]["reason"], r"largest \|q\| .* exceeds 0\.01")
        self.assertEqual(doc["settings"]["max_abs_q"], 0.01)

    def test_rerun_is_byte_identical(self):
        self.generate(self.root / "a.json.gz")
        self.generate(self.root / "b.json.gz")
        self.assertEqual((self.root / "a.json.gz").read_bytes(), (self.root / "b.json.gz").read_bytes())


if __name__ == "__main__":
    unittest.main()
