#!/usr/bin/env python3
"""Tests for the committed reference-run evaluation points and gradient counts."""

import contextlib
import gzip
import hashlib
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
import warnings
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import benchmark_points as bp
import toolchain


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def document(models=None, warmup=10, draws=10):
    return dict(schema=bp.SCHEMA,
                settings=dict(seed=1, warmup=warmup, draws=draws, chains=1, points=2),
                models=models or {})


BUILD = dict(stanli_build_id="build", stanli_abi_version=2)


def reference(stan="s", data="d", vectors=((0.5, -1.0), (2.0, 3.0)), iterations=(3, 14)):
    return dict(BUILD, source="reference_run", stan_sha256=digest(stan), data_sha256=digest(data),
                n_params=len(vectors[0]), points=[list(v) for v in vectors],
                iterations=list(iterations),
                gradients=dict(warmup=30, sampling=70, total=100),
                divergences=dict(warmup=1, sampling=0), max_treedepth=dict(warmup=0, sampling=2))


def fixed(stan="s", data="d", reason="reference run timed out after 60 s"):
    return dict(BUILD, source="fixed", stan_sha256=digest(stan), data_sha256=digest(data), reason=reason)


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

    def test_an_entry_from_another_build_or_abi_is_used_without_complaint(self):
        old = dict(reference(), stanli_build_id="abi1-0123456789ab-old", stanli_abi_version=1)
        old_fixed = dict(fixed(), stanli_build_id="abi1-0123456789ab-old", stanli_abi_version=1)
        self.write(document({"m": old, "f": old_fixed}))
        doc = bp.load(self.path)
        err = io.StringIO()
        with contextlib.redirect_stderr(err), warnings.catch_warnings():
            warnings.simplefilter("error")
            entry = bp.entry_for(doc, "m", digest("s"), digest("d"))
            self.assertEqual(bp.points_of(entry), [[0.5, -1.0], [2.0, 3.0]])
            self.assertEqual(bp.gradient_count(entry), 100)
            self.assertEqual(bp.entry_for(doc, "f", digest("s"), digest("d"))["source"], "fixed")
        self.assertEqual(err.getvalue(), "")

    def test_a_model_without_an_entry_is_an_error(self):
        self.write(document())
        with self.assertRaisesRegex(ValueError, "no entry.*gen_benchmark_points.py --missing"):
            bp.entry_for(bp.load(self.path), "m", digest("s"), digest("d"))

    def test_changed_hashes_point_at_the_missing_refresh(self):
        self.write(document({"m": reference()}))
        doc = bp.load(self.path)
        with self.assertRaisesRegex(ValueError, "stan source changed.*gen_benchmark_points.py --missing"):
            bp.entry_for(doc, "m", digest("other"), digest("d"))
        with self.assertRaisesRegex(ValueError, "data changed.*gen_benchmark_points.py --missing"):
            bp.entry_for(doc, "m", digest("s"), digest("other"))

    def test_the_committed_file_loads_with_a_build_on_every_entry(self):
        if not bp.DEFAULT_PATH.exists():
            self.skipTest("no committed points file")
        doc = bp.load(bp.DEFAULT_PATH)
        self.assertEqual(doc["schema"], 3)
        self.assertNotIn("stanli_build_id", doc["settings"])
        self.assertNotIn("stanli_abi_version", doc["settings"])
        for name, entry in doc["models"].items():
            with self.subTest(model=name):
                self.assertTrue(entry["stanli_build_id"])
                self.assertIsInstance(entry["stanli_abi_version"], int)

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
            "no build id": document({"m": {k: v for k, v in reference().items() if k != "stanli_build_id"}}),
            "empty build id": document({"m": with_(fixed(), stanli_build_id="")}),
            "no abi": document({"m": {k: v for k, v in fixed().items() if k != "stanli_abi_version"}}),
            "old schema": dict(document({"m": reference()}), schema=2),
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
        if not (REPO / "deps/posteriordb/posterior_database").is_dir():
            self.skipTest("posteriordb is not checked out")
        doc = bp.load(bp.DEFAULT_PATH)
        from corpus_inventory import corpus_cases, materialize_data
        cases = corpus_cases(REPO / "deps/posteriordb", "all")
        self.assertEqual(set(doc["models"]), set(cases))
        with tempfile.TemporaryDirectory() as scratch:
            for name, case in cases.items():
                bp.entry_for(doc, name, bp.sha256(case.source), bp.sha256(materialize_data(case, scratch)))


class _FakeLib:
    def __init__(self, build="new-build", abi=3):
        self.build, self.abi = build, abi

    def stanli_build_id(self):
        return self.build.encode()

    def stanli_abi_version(self):
        return self.abi


class MergeTests(unittest.TestCase):
    def setUp(self):
        import gen_benchmark_points as gen
        self.gen = gen
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.output = self.root / "points.json.gz"
        self.cases = {name: self.files(name) for name in ("a", "b", "c")}
        self.ran = []

    def files(self, name, stan=None, data="{}"):
        paths = self.root / f"{name}.stan", self.root / f"{name}.json"
        paths[0].write_text(stan or f"// {name}")
        paths[1].write_text(data)
        return paths

    def fake_job(self, name, stan, data, lib, probe, seed, warmup, draws, timeout, points):
        self.ran.append(name)
        base = dict(stan_sha256=bp.sha256(stan), data_sha256=bp.sha256(data))
        if name == "broken":
            return name, dict(base, error="stanc failed: nope")
        return name, dict(base, n_params=1, iterations=list(range(points)),
                          points=[[float(seed + k)] for k in range(points)],
                          gradients=dict(warmup=warmup, sampling=draws, total=warmup + draws),
                          divergences=dict(warmup=0, sampling=0), max_treedepth=dict(warmup=0, sampling=0))

    def generate(self, cases=None, build="new-build", **options):
        settings = dict(seed=1, warmup=4, draws=6, jobs=2, timeout=60, points=3)
        settings.update(options)
        with mock.patch.object(self.gen, "_bind", return_value=_FakeLib(build)), \
                mock.patch.object(self.gen, "_model_job", self.fake_job), \
                contextlib.redirect_stderr(io.StringIO()) as err:
            self.gen.generate(cases or self.cases, lib="unused", probe="unused", output=self.output,
                              **settings)
        self.stderr = err.getvalue()
        return bp.load(self.output)

    def hashes(self, cases=None):
        return {n: (bp.sha256(s), bp.sha256(d)) for n, (s, d) in (cases or self.cases).items()}

    def test_a_full_run_records_the_build_on_every_entry_and_not_in_settings(self):
        doc = self.generate()
        self.assertEqual(doc["schema"], 3)
        self.assertEqual(sorted(doc["models"]), ["a", "b", "c"])
        self.assertNotIn("stanli_build_id", doc["settings"])
        self.assertNotIn("stanli_abi_version", doc["settings"])
        for entry in doc["models"].values():
            self.assertEqual((entry["stanli_build_id"], entry["stanli_abi_version"]), ("new-build", 3))

    def test_a_failed_entry_records_the_build_too(self):
        cases = dict(self.cases, broken=self.files("broken"))
        entry = self.generate(cases)["models"]["broken"]
        self.assertEqual((entry["source"], entry["stanli_build_id"]), ("fixed", "new-build"))

    def test_merge_keeps_unselected_entries(self):
        before = self.generate(build="old-build")
        self.ran.clear()
        after = self.generate(selected=["b"], build="new-build", seed=1)
        self.assertEqual(self.ran, ["b"])
        self.assertEqual(after["models"]["a"], before["models"]["a"])
        self.assertEqual(after["models"]["c"], before["models"]["c"])
        self.assertEqual(after["models"]["b"]["stanli_build_id"], "new-build")
        self.assertEqual(after["models"]["a"]["stanli_build_id"], "old-build")

    def test_models_gone_from_the_corpus_are_dropped_and_listed(self):
        self.generate(dict(self.cases, gone=self.files("gone")))
        doc = self.generate(selected=["a"])
        self.assertEqual(sorted(doc["models"]), ["a", "b", "c"])
        self.assertIn("gone", self.stderr)

    def test_a_settings_mismatch_refuses_before_running_anything(self):
        self.generate()
        before = self.output.read_bytes()
        self.ran.clear()
        for change in (dict(seed=2), dict(warmup=5), dict(timeout=90), dict(points=4)):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "settings"):
                self.generate(selected=["a"], **change)
        self.assertEqual(self.ran, [])
        self.assertEqual(self.output.read_bytes(), before)

    def test_a_build_change_alone_merges(self):
        self.generate(build="old-build")
        self.assertEqual(self.generate(selected=["a"], build="new-build")["models"]["a"]["stanli_build_id"],
                         "new-build")

    def test_a_full_refresh_may_change_settings(self):
        self.generate()
        doc = self.generate(seed=5)
        self.assertEqual(doc["settings"]["seed"], 5)
        self.assertEqual(doc["models"]["a"]["points"][0], [5.0])

    def test_missing_selects_absent_and_changed_models_only(self):
        self.generate({k: self.cases[k] for k in ("a", "b", "c")})
        cases = dict(self.cases, new=self.files("new"))
        cases["b"] = self.files("b", stan="// edited")
        cases["c"] = self.files("c", data='{"N": 1}')
        doc = bp.load(self.output)
        self.assertEqual(self.gen.select_models(doc, self.hashes(cases), missing=True), ["b", "c", "new"])

    def test_missing_skips_a_matching_fixed_entry(self):
        cases = dict(self.cases, broken=self.files("broken"))
        self.generate(cases)
        self.assertEqual(self.gen.select_models(bp.load(self.output), self.hashes(cases), missing=True), [])

    def test_missing_without_a_file_selects_everything(self):
        self.assertEqual(self.gen.select_models(None, self.hashes(), missing=True), ["a", "b", "c"])

    def test_models_selects_exact_names_and_rejects_unknown_ones(self):
        hashes = self.hashes()
        self.assertEqual(self.gen.select_models(None, hashes, models=["c", "a"]), ["a", "c"])
        with self.assertRaisesRegex(ValueError, "unknown model.*ab"):
            self.gen.select_models(None, hashes, models=["ab"])

    def test_filter_and_no_selector(self):
        hashes = dict(self.hashes(), ab=("0" * 64, "0" * 64))
        self.assertEqual(self.gen.select_models(None, hashes, filter="a"), ["a", "ab"])
        self.assertEqual(self.gen.select_models(None, hashes), ["a", "ab", "b", "c"])

    def test_the_command_line_takes_one_selector_and_comma_separated_models(self):
        self.assertEqual(self.gen.parse_args(["--lib", "x", "--models", "a,b", "--models", "c"])[1].models,
                         ["a", "b", "c"])
        for extra in (["--missing", "--models", "a"], ["--filter", "a", "--missing"], ["--models", ","]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit):
                self.gen.parse_args(["--lib", "x", *extra])


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
