#!/usr/bin/env python3
"""Focused tests for the corpus benchmark's CmdStan build cache and build pre-pass."""

import contextlib
import io
import shutil
import json
import os
import pathlib
import sys
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import test_corpus_bench as existing
from benchmark_artifact_fixture import artifact_fixture, write_artifacts
from harnesses import corpus_bench
from harnesses.corpus_bench import CmdstanCache, Runner, cache_context, cmdstan_build, prebuild

STAN = "a" * 64


def fake_compile(cmdstan, hpp, driver, exe, opt="-O1"):
    return ["clang++", opt, f"-I{cmdstan}", "-include", str(hpp), str(driver), "-o", str(exe)]


def fake_args(root, **changes):
    values = dict(cmdstan=root / "cmdstan", stanc=root / "stanc", stancflags="")
    values.update(changes)
    return SimpleNamespace(**values)


def fake_expected():
    return dict(
        executables=dict(bench="bench", vectorize_probe="probe", stanc="stanc-sha"),
        sources={"harnesses/corpus_bench.py": "harness", "tools/bench_grad.cpp": "grad",
                 "tools/bench_cmdstan_grad.cpp": "driver", "tools/benchmark_timer.hpp": "timer"},
        toolchains=dict(cmdstan={"head": "c", "tracked_diff_sha256": "cd"},
                        stan={"head": "s", "tracked_diff_sha256": "sd"},
                        math={"head": "m", "tracked_diff_sha256": "md"},
                        stanli_math={"head": "x", "tracked_diff_sha256": "xd"},
                        libraries={"tbb/libtbb.dylib": "tbb"}, clang_sha256="clang",
                        build_environment={"CXX": None, "CXXFLAGS": None}))


class FakeRunner:
    """Executes nothing: writes the files each build phase would and records every call."""
    ELAPSED = {"stanc-cpp": .5, "gradient-driver-build": 8., "cmdstan-build": 3.}

    def __init__(self, delay=0.):
        self.calls, self.fail, self.hooks = [], {}, {}
        self.delay, self.lock = delay, threading.Lock()
        self.running = self.peak = 0
        self.intervals = []

    def kill_active(self):
        self.killed = True

    killed = False

    def run(self, phase, argv, timeout, cwd=None):
        step = phase.split("/")[-1]
        with self.lock:
            event_id = len(self.calls)
            self.calls.append(dict(phase=phase, argv=[str(a) for a in argv], cwd=cwd))
            self.running += 1
            self.peak = max(self.peak, self.running)
        began = time.monotonic()
        if self.delay:
            threading.Event().wait(self.delay)
        status = self.fail.get(phase, "ok")
        if status == "ok":
            if step == "stanc-cpp":
                output = next(a for a in argv if str(a).startswith("--o="))[4:]
                (pathlib.Path(cwd) / output).write_text(f"// stanc read {argv[1]}\n")
            elif step == "gradient-driver-build":
                (pathlib.Path(cwd) / argv[-1]).write_text("driver " + phase)
            elif step == "cmdstan-build":
                pathlib.Path(argv[1]).write_text("executable " + phase)
        if phase in self.hooks:
            self.hooks[phase]()
        with self.lock:
            self.running -= 1
            self.intervals.append((phase, began, time.monotonic()))
        return dict(id=event_id, phase=phase, argv=[str(a) for a in argv], status=status,
                    returncode=0 if status == "ok" else 1, elapsed_s=self.ELAPSED.get(step, 1.))


class BuildFixture(unittest.TestCase):
    def setUp(self):
        from bench_build_cache import BuildCache
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / "cmdstan").mkdir()
        self.args = SimpleNamespace(cmdstan=self.root / "cmdstan", stanc=self.root / "stanc",
                                    stancflags="--O1", build_timeout=900)
        self.stan = self.root / "m.stan"
        self.stan.write_text("parameters { real x; } model { x ~ normal(0, 1); }")
        patch = mock.patch("harnesses.corpus_bench.compile_cmd", side_effect=fake_compile)
        patch.start()
        self.addCleanup(patch.stop)
        self.store = BuildCache(self.root / "cache")
        self.cache = CmdstanCache(self.store, cache_context(self.args, fake_expected()), "run-1")
        self.runner = FakeRunner()

    def build(self, name="m", cache="default", runner=None, stan=None):
        work = self.root / "run" / name
        work.mkdir(parents=True, exist_ok=True)
        return cmdstan_build(name, stan or self.stan, work, runner or self.runner, self.args,
                             self.cache if cache == "default" else cache)

    def key(self, stan=None):
        return self.cache.key(stan or self.stan)

    def driver_key(self, stan=None):
        return self.cache.driver_key(stan or self.stan)

    def entries(self):
        return sorted(p.name for p in self.store.root.iterdir()) if self.store.root.exists() else []


class BuildCacheOrchestrationTests(BuildFixture):
    def test_miss_builds_in_the_entry_from_a_stable_relative_path(self):
        build = self.build()
        key, dkey = self.key(), self.driver_key()
        entry, driver = self.store.entry(key), self.store.entry(dkey)
        self.assertEqual(self.entries(), sorted([key, dkey]))
        self.assertEqual(build["cache"], dict(directory=str(self.store.root), key=key, driver_key=dkey,
                                              hit=False, model_hit=False, driver_hit=False))
        self.assertEqual(sorted(p.name for p in entry.iterdir()),
                         ["meta.json", "model", "model.hpp", "model.stan"])
        self.assertEqual(sorted(p.name for p in driver.iterdir()), ["gradbench", "meta.json"])
        self.assertEqual((entry / "model.stan").read_bytes(), self.stan.read_bytes())
        self.assertEqual(build["gradient"], driver / "gradbench")
        self.assertEqual(build["executable"], entry / "model")
        stanc = self.runner.calls[0]
        self.assertEqual(stanc["argv"][1:3], ["model.stan", "--o=model.hpp"])
        for call in self.runner.calls[:2]:
            self.assertTrue(str(call["cwd"]).startswith(str(self.store.root)))
        self.assertIn("model.hpp", self.runner.calls[1]["argv"])
        self.assertNotIn(str(self.root / "run"), json.dumps(self.runner.calls, default=str))
        self.assertEqual((entry / "model.hpp").read_text(), "// stanc read model.stan\n")
        self.assertEqual([c["phase"] for c in self.runner.calls],
                         ["m/stanc-cpp", "m/gradient-driver-build", "m/cmdstan-build"])
        self.assertEqual(self.runner.calls[2]["cwd"], (self.root / "cmdstan").resolve())

    def test_metadata_records_original_durations_hashes_and_builder(self):
        self.build()
        meta = self.store.lookup(self.key())
        self.assertEqual((meta["stanc_s"], meta["build_s"]), (.5, 3.))
        self.assertNotIn("driver_build_s", meta)
        self.assertEqual(meta["run_id"], "run-1")
        self.assertRegex(meta["created_utc"], r"^\d{4}-\d\d-\d\dT")
        entry = self.store.entry(self.key())
        self.assertEqual(meta["executable_sha256"], corpus_bench.sha(entry / "model"))
        driver_meta = self.store.lookup(self.driver_key())
        self.assertEqual(driver_meta["driver_build_s"], 8.)
        self.assertEqual(driver_meta["model_key"], self.key())
        self.assertEqual(driver_meta["run_id"], "run-1")
        self.assertEqual(driver_meta["driver_sha256"],
                         corpus_bench.sha(self.store.entry(self.driver_key()) / "gradbench"))

    def test_hit_runs_no_commands_and_marks_the_reused_setup_events(self):
        self.build()
        again = FakeRunner()
        self.cache.run_id = "run-2"
        build = self.build("copy", runner=again)
        key = self.key()
        self.assertEqual(again.calls, [])
        self.assertEqual(build["cache"], dict(directory=str(self.store.root), key=key,
                                              driver_key=self.driver_key(), hit=True, model_hit=True,
                                              driver_hit=True, run_id="run-1", driver_run_id="run-1"))
        self.assertEqual(build["driver_build_s"], 8.)
        self.assertEqual(build["gradient"], self.store.entry(self.driver_key()) / "gradbench")
        stanc, make = build["setup"]["cmdstan_stanc"], build["setup"]["cmdstan_build"]
        self.assertEqual((stanc["phase"], stanc["elapsed_s"]), ("copy/stanc-cpp", .5))
        self.assertEqual((make["phase"], make["elapsed_s"]), ("copy/cmdstan-build", 3.))
        for event in (stanc, make):
            self.assertEqual((event["status"], event["returncode"], event["cached"]), ("ok", 0, True))
            self.assertEqual((event["cache_key"], event["cache_run_id"]), (key, "run-1"))
        self.assertIsNone(build["gradient_failure"])
        self.assertIsNone(build["build_failure"])

    def test_edited_source_is_a_different_entry(self):
        self.build()
        other = self.root / "other.stan"
        other.write_text("parameters { real y; } model { y ~ normal(0, 1); }")
        self.build("other", stan=other)
        self.assertEqual(len(self.entries()), 4)

    def test_without_a_cache_the_build_stays_in_the_run_directory(self):
        build = self.build(cache=None)
        work = self.root / "run" / "m"
        self.assertEqual(build["cache"], dict(directory=None, hit=False))
        self.assertEqual(build["gradient"], work / "gradbench")
        self.assertEqual(build["executable"], work / "m")
        self.assertEqual(self.runner.calls[0]["cwd"], work)
        self.assertEqual(self.runner.calls[0]["argv"][1:3], ["m.stan", "--o=m.hpp"])
        self.assertFalse(self.store.root.exists())
        self.assertEqual(self.build(cache=None)["setup"]["cmdstan_build"]["elapsed_s"], 3.)
        self.assertNotIn("cached", build["setup"]["cmdstan_build"])

    def test_failed_make_leaves_no_entry_but_keeps_the_driver_for_this_run(self):
        self.runner.fail["m/cmdstan-build"] = "failed"
        build = self.build()
        self.assertEqual(self.entries(), [])
        self.assertIsNone(build["gradient_failure"])
        self.assertRegex(build["build_failure"], r"^m/cmdstan-build: failed \(event 2\)$")
        self.assertEqual(build["setup"]["cmdstan_build"]["status"], "failed")
        self.assertEqual(build["gradient"].read_text(), "driver m/gradient-driver-build")
        self.assertTrue(build["gradient"].is_relative_to(self.root / "run" / "m"))
        self.assertEqual(build["cache"]["hit"], False)
        self.assertIsNone(build["executable"])

    def test_failed_stanc_or_driver_leaves_no_entry_and_no_scratch(self):
        for phase in ("m/stanc-cpp", "m/gradient-driver-build"):
            with self.subTest(phase=phase):
                self.runner = FakeRunner()
                self.runner.fail[phase] = "timeout"
                shutil.rmtree(self.root / "run", ignore_errors=True)
                build = self.build()
                self.assertEqual(self.entries(), [])
                self.assertRegex(build["gradient_failure"], rf"^{phase}: timeout \(event \d\)$")
                self.assertIsNone(build["gradient"])

    def test_losing_the_race_keeps_the_winners_entry_and_our_fresh_timings(self):
        key = self.key()
        def winner():
            scratch = self.store.scratch(key)
            for name in ("model.stan", "model.hpp", "model"):
                (scratch / name).write_text("winner " + name)
            self.store.commit(scratch, key, dict(files=["model.stan", "model.hpp", "model"],
                                                 run_id="other-run", stanc_s=9., build_s=9.))
        self.runner.hooks["m/cmdstan-build"] = winner
        build = self.build()
        self.assertEqual(self.entries(), sorted([key, self.driver_key()]))
        self.assertEqual((self.store.entry(key) / "model").read_text(), "winner model")
        self.assertEqual(build["executable"], self.store.entry(key) / "model")
        self.assertEqual(build["cache"]["hit"], False)
        self.assertEqual(build["cache"]["raced"], True)
        self.assertEqual(build["setup"]["cmdstan_build"]["elapsed_s"], 3.)
        self.assertNotIn("cached", build["setup"]["cmdstan_build"])


class SplitCacheTests(BuildFixture):
    def rebuilt_with(self, driver="driver2"):
        expected = fake_expected()
        expected["sources"]["tools/bench_cmdstan_grad.cpp"] = driver
        self.cache = CmdstanCache(self.store, cache_context(self.args, expected), "run-2")

    def test_a_driver_change_rebuilds_only_the_driver(self):
        first = self.build()
        self.rebuilt_with()
        again = FakeRunner()
        build = self.build("copy", runner=again)
        self.assertEqual([c["phase"] for c in again.calls], ["copy/gradient-driver-build"])
        self.assertEqual(build["cache"], dict(directory=str(self.store.root), key=self.key(),
                                              driver_key=self.driver_key(), hit=False, model_hit=True,
                                              driver_hit=False, run_id="run-1"))
        self.assertEqual(build["executable"], first["executable"])
        self.assertNotEqual(build["gradient"], first["gradient"])
        self.assertEqual(build["driver_build_s"], 8.)
        self.assertEqual(len(self.entries()), 3)
        self.assertEqual(build["setup"]["cmdstan_stanc"]["cached"], True)
        self.assertEqual(build["setup"]["cmdstan_build"]["elapsed_s"], 3.)
        self.assertEqual(self.store.lookup(self.driver_key())["run_id"], "run-2")

    def test_the_rebuilt_driver_compiles_against_the_cached_header(self):
        self.build()
        self.rebuilt_with()
        again = FakeRunner()
        self.build("copy", runner=again)
        cwd = pathlib.Path(again.calls[0]["cwd"])
        self.assertTrue(str(cwd).startswith(str(self.store.root)))
        self.assertIn("model.hpp", again.calls[0]["argv"])
        self.assertEqual(sorted(p.name for p in self.store.entry(self.driver_key()).iterdir()),
                         ["gradbench", "meta.json"])
        self.assertEqual([p for p in self.store.root.iterdir() if p.name.startswith(".tmp-")], [])

    def test_a_failed_driver_rebuild_keeps_the_model_entry_and_fails_the_gradient(self):
        self.build()
        self.rebuilt_with()
        again = FakeRunner()
        again.fail["copy/gradient-driver-build"] = "failed"
        build = self.build("copy", runner=again)
        self.assertRegex(build["gradient_failure"], r"^copy/gradient-driver-build: failed")
        self.assertIsNone(build["gradient"])
        self.assertEqual(len(self.entries()), 2)
        self.assertIsNone(build["build_failure"])

    def test_a_model_change_rebuilds_everything(self):
        self.build()
        again = FakeRunner()
        self.args.stancflags = "--O1 --other"
        self.cache = CmdstanCache(self.store, cache_context(self.args, fake_expected()), "run-2")
        build = self.build("copy", runner=again)
        self.assertEqual([c["phase"].split("/")[-1] for c in again.calls],
                         ["stanc-cpp", "gradient-driver-build", "cmdstan-build"])
        self.assertEqual(build["cache"]["model_hit"], False)
        self.assertEqual(len(self.entries()), 4)


class PrebuiltMeasureTests(existing.SetupEstimateTests):
    """Every outcome the inline build produces is the same when the build ran earlier."""
    def measure(self, points=None):
        work = self.root / "m"
        with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]), \
             mock.patch("harnesses.corpus_bench.sha", return_value="test-hash"):
            build = cmdstan_build("m", self.source, work, self.runner, self.args)
            self.prebuild_events = len(self.events)
            return corpus_bench.measure_model("m", self.source, self.data, self.root,
                                              {"run_id": "test-run"}, self.runner, self.args, build,
                                              points)

    def test_source_compile_failure_keeps_event_without_setup_estimate(self):
        self.failures["m/stanli-mir"] = "failed"
        record = self.measure()
        self.assertEqual(len(self.events), self.prebuild_events + 1)
        self.assertEqual(set(record["setup"]), {"stanli_compile"})
        self.assertEqual(record["setup"]["stanli_compile"]["status"], "failed")
        self.assertNotIn("cmdstan_stanc_s", record["row"])
        self.assertNotIn("stanli_estimated_s", record["row"])

    def test_failed_stanc_and_driver_builds_fail_the_row_in_order(self):
        for phase, expected in (("m/stanc-cpp", "m/stanc-cpp: failed"),
                                ("m/gradient-driver-build", "m/gradient-driver-build: failed")):
            with self.subTest(phase=phase):
                self.events.clear()
                self.failures = {phase: "failed"}
                record = self.measure()
                self.assertEqual(record["status"], "failed")
                self.assertTrue(record["row"]["note"].startswith(expected), record["row"]["note"])
                self.assertEqual(record["gradients"], [])
                self.assertNotIn("cmdstan_ns_grad", record["row"])

    def test_timed_out_driver_build_is_censored(self):
        self.failures["m/gradient-driver-build"] = "timeout"
        self.assertEqual(self.measure()["status"], "censored")

    def test_cached_setup_supplies_the_published_numbers_and_the_hit_is_recorded(self):
        from bench_build_cache import BuildCache
        with mock.patch("harnesses.corpus_bench.compile_cmd", side_effect=fake_compile):
            cache = CmdstanCache(BuildCache(self.root / "cache"),
                                 cache_context(self.args, fake_expected()), "older-run")
            cmdstan_build("m", self.source, self.root / "first", FakeRunner(), self.args, cache)
            cache.run_id = "this-run"
            build = cmdstan_build("m", self.source, self.root / "m", self.runner, self.args, cache)
        self.assertEqual(self.events, [])
        with mock.patch("harnesses.corpus_bench.compile_cmd", return_value=["c++"]), \
             mock.patch("harnesses.corpus_bench.sha", return_value="test-hash"):
            record = corpus_bench.measure_model("m", self.source, self.data, self.root,
                                                {"run_id": "this-run"}, self.runner, self.args, build)
        self.assertEqual(record["status"], "ok")
        self.assertEqual((record["row"]["cmdstan_stanc_s"], record["row"]["cmdstan_build_s"]), (.5, 3.))
        self.assertEqual(record["row"]["cmdstan_estimated_s"], .5 + 3. + 20000 * 500000 / 1e9)
        for key in ("cmdstan_stanc", "cmdstan_build"):
            self.assertEqual(record["setup"][key]["cached"], True)
            self.assertEqual(record["setup"][key]["cache_run_id"], "older-run")
        self.assertNotIn("cached", record["setup"]["stanli_compile"])
        self.assertEqual(record["build_cache"]["hit"], True)
        self.assertEqual(record["build_cache"]["key"], cache.key(self.source))
        self.assertEqual(record["build_cache"]["driver_key"], cache.driver_key(self.source))
        phases = [event["phase"].split("/")[-1] for event in self.events]
        self.assertNotIn("stanc-cpp", phases)
        self.assertNotIn("cmdstan-build", phases)
        self.assertNotIn("gradient-driver-build", phases)
        self.assertEqual(json.loads((self.root / "m.result.json").read_text()), record)


class PrebuildTests(BuildFixture):
    def setUp(self):
        super().setUp()
        self.args.build_jobs = 2
        self.inputs = {}
        for name in ("a", "b", "c", "d", "e"):
            stan = self.root / f"{name}.stan"
            stan.write_text(f"parameters {{ real {name}; }} model {{ {name} ~ normal(0, 1); }}")
            self.inputs[name] = (stan, self.root / f"{name}.json")

    def prebuild(self, cache="default", runner=None, inputs=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return prebuild(inputs or self.inputs, self.root / "run", runner or self.runner, self.args,
                            self.cache if cache == "default" else cache)

    def test_builds_every_model_without_exceeding_the_job_limit(self):
        self.runner = FakeRunner(delay=.03)
        builds = self.prebuild()
        self.assertEqual(sorted(builds), list(self.inputs))
        self.assertEqual(self.runner.peak, 2)
        for build in builds.values():
            self.assertTrue(build["gradient"].is_file())
            self.assertEqual(build["cache"]["hit"], False)
        self.assertEqual(len(self.entries()), 10)

    def test_published_setup_events_never_overlap_another_command(self):
        self.runner = FakeRunner(delay=.03)
        self.prebuild()
        published = [i for i in self.runner.intervals
                     if i[0].split("/")[-1] in ("stanc-cpp", "cmdstan-build")]
        self.assertEqual(len(published), 10)
        for mine in published:
            for other in self.runner.intervals:
                if other is not mine:
                    self.assertTrue(other[2] <= mine[1] or other[1] >= mine[2], (mine, other))

    def test_only_driver_builds_run_in_parallel_between_serial_stanc_and_make(self):
        self.runner = FakeRunner(delay=.03)
        self.prebuild()
        end = lambda step: max(i[2] for i in self.runner.intervals if i[0].endswith(step))
        start = lambda step: min(i[1] for i in self.runner.intervals if i[0].endswith(step))
        self.assertLessEqual(end("stanc-cpp"), start("gradient-driver-build"))
        self.assertLessEqual(end("gradient-driver-build"), start("cmdstan-build"))
        drivers = sorted(i for i in self.runner.intervals if i[0].endswith("gradient-driver-build"))
        self.assertTrue(any(a[2] > b[1] for a, b in zip(drivers, drivers[1:])))

    def test_runs_only_cmdstan_build_phases(self):
        self.prebuild()
        steps = {call["phase"].split("/")[-1] for call in self.runner.calls}
        self.assertEqual(steps, {"stanc-cpp", "gradient-driver-build", "cmdstan-build"})
        self.assertEqual(len(self.runner.calls), 15)

    def test_one_models_failure_or_timeout_does_not_stop_the_others(self):
        self.runner.fail.update({"b/stanc-cpp": "failed", "c/cmdstan-build": "timeout",
                                 "d/gradient-driver-build": "failed"})
        builds = self.prebuild()
        self.assertEqual(sorted(builds), list(self.inputs))
        self.assertRegex(builds["b"]["gradient_failure"], r"^b/stanc-cpp: failed \(event \d+\)$")
        self.assertRegex(builds["c"]["build_failure"], r"^c/cmdstan-build: timeout \(event \d+\)$")
        self.assertRegex(builds["d"]["gradient_failure"], r"^d/gradient-driver-build: failed")
        for name in ("a", "e"):
            self.assertIsNone(builds[name]["gradient_failure"])
            self.assertIsNone(builds[name]["build_failure"])
        self.assertEqual(self.entries(), sorted(k(self.inputs[n][0]) for n in ("a", "e")
                                                for k in (self.cache.key, self.cache.driver_key)))

    def test_an_unexpected_error_in_one_build_is_that_models_failure(self):
        self.inputs["c"][0].unlink()
        builds = self.prebuild()
        self.assertRegex(builds["c"]["gradient_failure"], r"^c/cmdstan-build: error \(")
        self.assertIsNone(builds["a"]["gradient_failure"])
        self.assertIsNone(builds["e"]["gradient_failure"])

    def test_models_sharing_a_source_build_once_and_the_rest_reuse_the_entry(self):
        self.inputs["twin"] = (self.inputs["a"][0], self.root / "twin.json")
        builds = self.prebuild()
        stanc = [c for c in self.runner.calls if c["phase"].endswith("/stanc-cpp")]
        self.assertEqual(len(stanc), 5)
        self.assertEqual(builds["a"]["cache"]["hit"], False)
        self.assertEqual(builds["twin"]["cache"]["hit"], True)
        self.assertEqual(builds["twin"]["cache"]["run_id"], "run-1")
        self.assertEqual(builds["twin"]["gradient"], builds["a"]["gradient"])
        self.assertEqual(builds["twin"]["setup"]["cmdstan_stanc"]["phase"], "twin/stanc-cpp")

    def test_a_failed_shared_source_is_retried_by_its_twin_not_assumed_cached(self):
        self.inputs["twin"] = (self.inputs["a"][0], self.root / "twin.json")
        self.runner.fail["a/stanc-cpp"] = "failed"
        builds = self.prebuild()
        self.assertEqual(builds["twin"]["cache"]["hit"], False)
        self.assertIsNone(builds["twin"]["gradient_failure"])

    def test_without_a_cache_models_build_in_parallel_in_the_run_directory(self):
        self.runner = FakeRunner(delay=.03)
        builds = self.prebuild(cache=None)
        self.assertEqual(self.runner.peak, 2)
        self.assertFalse(self.store.root.exists())
        self.assertEqual(builds["a"]["gradient"], self.root / "run" / "a" / "gradbench")
        self.assertEqual({b["cache"]["directory"] for b in builds.values()}, {None})

    def test_second_pass_over_the_same_sources_runs_no_commands(self):
        self.prebuild()
        again = FakeRunner()
        builds = self.prebuild(runner=again)
        self.assertEqual(again.calls, [])
        self.assertEqual({b["cache"]["hit"] for b in builds.values()}, {True})

    def test_a_driver_change_reports_the_model_as_cached_and_rebuilds_only_drivers(self):
        self.prebuild()
        expected = fake_expected()
        expected["sources"]["tools/benchmark_timer.hpp"] = "newer"
        self.cache = CmdstanCache(self.store, cache_context(self.args, expected), "run-2")
        again = FakeRunner()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            builds = prebuild(self.inputs, self.root / "run", again, self.args, self.cache)
        self.assertEqual({c["phase"].split("/")[-1] for c in again.calls}, {"gradient-driver-build"})
        self.assertEqual(len(again.calls), 5)
        self.assertEqual({(b["cache"]["model_hit"], b["cache"]["driver_hit"]) for b in builds.values()},
                         {(True, False)})
        self.assertIn("a: cmdstan build model cached, driver rebuilt", out.getvalue())
        self.assertEqual(len(self.entries()), 15)

    def test_interrupt_stops_running_commands_and_propagates(self):
        self.runner.hooks["b/stanc-cpp"] = lambda: (_ for _ in ()).throw(KeyboardInterrupt())
        with self.assertRaises(KeyboardInterrupt):
            self.prebuild()
        self.assertTrue(self.runner.killed)


class DirectoryRunner(FakeRunner):
    def __init__(self, directory):
        super().__init__()
        self.directory = directory


class MainWiringTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        for name in ("bench", "stanc", "probe"):
            (self.root / name).write_text(name)
        for name in ("cmdstan", "pdb", "home"):
            (self.root / name).mkdir()
        self.cases = {}
        for name in ("a", "b"):
            source, data = self.root / f"{name}.stan", self.root / f"{name}.json"
            source.write_text(f"parameters {{ real {name}; }} model {{ {name} ~ normal(0, 1); }}")
            data.write_text("{}")
            self.cases[name] = SimpleNamespace(source=source, data=data, collection="posteriordb")
        self.runners, self.measured, self.points = [], [], {}
        environment = {"PATH": os.environ["PATH"], "HOME": str(self.root / "home"),
                       "USERPROFILE": str(self.root / "home")}
        for patch in (mock.patch.dict(os.environ, environment, clear=True),
                      mock.patch.object(corpus_bench.shutil, "which", lambda name: str(self.root / "stanc")),
                      mock.patch.object(corpus_bench, "corpus_cases", lambda *a: self.cases),
                      mock.patch.object(corpus_bench, "checkout_identity",
                                        lambda path: dict(head="h", tracked_diff_sha256="d")),
                      mock.patch.object(corpus_bench, "library_identity", lambda path: {}),
                      mock.patch.object(corpus_bench, "compile_cmd", fake_compile),
                      mock.patch.object(corpus_bench, "Runner", self.make_runner),
                      mock.patch.object(corpus_bench, "measure_model", self.measure)):
            patch.start()
            self.addCleanup(patch.stop)

    def make_runner(self, directory):
        runner = DirectoryRunner(directory)
        self.runners.append(runner)
        return runner

    def measure(self, name, stan, data, directory, manifest, runner, args, build=None, points=None):
        self.measured.append((name, build, len(runner.calls)))
        self.points[name] = points
        record = dict(model=name, status="ok", row=dict(model=name, note=""))
        (directory / f"{name}.result.json").write_text(json.dumps(record))
        return record

    def run_main(self, *extra, output="out.tsv"):
        with contextlib.redirect_stdout(io.StringIO()):
            status = corpus_bench.main([str(self.root / "cmdstan"), str(self.root / "pdb"),
                str(self.root / output), "--stanc", str(self.root / "stanc"),
                "--bench", str(self.root / "bench"), "--vectorize-probe", str(self.root / "probe"),
                *extra])
        return status, pathlib.Path(str(self.root / output) + ".run")

    def prior_file(self, **changes):
        import benchmark_points as bp
        stan, data = self.cases["a"].source, self.cases["a"].data
        stan.write_text("data { int prior_only; }\nparameters { real a; }\nmodel { a ~ normal(0, 1); }\n")
        entry = dict(source="prior", stan_sha256=bp.sha256(stan), data_sha256=bp.sha256(data),
                     n_params=1, points=[[0.1 * k] for k in range(10)])
        path = self.root / "points.json.gz"
        bp.write(path, dict(schema=bp.SCHEMA, settings=dict(selected_draws=list(range(10))),
                            models={"a": dict(entry, **changes)}, fallbacks={}))
        return path

    def test_prior_points_are_frozen_in_the_run_and_recorded_in_the_manifest(self):
        status, run = self.run_main("--no-build-cache", "--prior-points", str(self.prior_file()))
        self.assertEqual(status, 0)
        frozen = run / "inputs" / "a.points.json"
        self.assertEqual(json.loads(frozen.read_text()), [[0.1 * k] for k in range(10)])
        self.assertEqual(self.points, {"a": frozen, "b": None})
        inputs = json.loads((run / "manifest.json").read_text())["identity"]["inputs"]
        self.assertEqual((inputs["a"]["point_source"], inputs["b"]["point_source"]), ("prior", "fixed"))
        self.assertEqual(inputs["a"]["points_sha256"], corpus_bench.sha(frozen))
        self.assertNotIn("points_sha256", inputs["b"])

    def test_no_prior_points_uses_fixed_points_for_every_model(self):
        status, run = self.run_main("--no-build-cache", "--prior-points", str(self.prior_file()),
                                    "--no-prior-points")
        self.assertEqual(status, 0)
        self.assertEqual(self.points, {"a": None, "b": None})
        inputs = json.loads((run / "manifest.json").read_text())["identity"]["inputs"]
        self.assertEqual({v["point_source"] for v in inputs.values()}, {"fixed"})

    def test_a_points_file_that_no_longer_matches_the_source_is_an_error(self):
        path = self.prior_file(stan_sha256="0" * 64)
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()) as err:
            self.run_main("--no-build-cache", "--prior-points", str(path))
        self.assertIn("stan source changed", err.getvalue())
        self.assertEqual(self.measured, [])

    def test_every_build_finishes_before_the_first_measurement(self):
        cache = self.root / "cache"
        status, run = self.run_main("--build-cache", str(cache), "--build-jobs", "3")
        self.assertEqual(status, 0)
        self.assertEqual([m[2] for m in self.measured], [6, 6])
        self.assertEqual(len(list(cache.iterdir())), 4)
        for name, build, _ in self.measured:
            self.assertEqual(build["cache"]["hit"], False)
            self.assertTrue(build["gradient"].is_file())
        (line,) = (run / "build-prepass.jsonl").read_text().splitlines()
        evidence = json.loads(line)
        self.assertEqual((evidence["jobs"], evidence["cache_directory"]), (3, str(cache)))
        self.assertEqual({m: v["hit"] for m, v in evidence["models"].items()}, {"a": False, "b": False})

    def test_warm_run_hits_every_model_and_runs_no_cmdstan_commands(self):
        cache = self.root / "cache"
        self.run_main("--build-cache", str(cache))
        self.measured.clear()
        self.run_main("--build-cache", str(cache), output="again.tsv")
        self.assertEqual(self.runners[1].calls, [])
        self.assertEqual([m[1]["cache"]["hit"] for m in self.measured], [True, True])
        run = pathlib.Path(str(self.root / "again.tsv") + ".run")
        evidence = json.loads((run / "build-prepass.jsonl").read_text())
        self.assertEqual({v["hit"] for v in evidence["models"].values()}, {True})

    def test_cache_options_are_not_part_of_the_frozen_identity(self):
        _, first = self.run_main("--build-cache", str(self.root / "cache"), "--build-jobs", "2")
        _, second = self.run_main("--no-build-cache", output="plain.tsv")
        configs = [json.loads((run / "manifest.json").read_text())["identity"]["config"]
                   for run in (first, second)]
        self.assertEqual(configs[0], configs[1])
        for key in ("build_cache", "no_build_cache", "build_jobs"):
            self.assertNotIn(key, configs[0])

    def test_default_location_prefers_the_environment_then_the_home_cache(self):
        home_cache = self.root / "home/.cache/stanli/corpus-bench"
        self.run_main()
        self.assertEqual(len(list(home_cache.iterdir())), 4)
        with mock.patch.dict(os.environ, {"STANLI_BENCH_CACHE": str(self.root / "env-cache")}):
            self.run_main(output="env.tsv")
        self.assertEqual(len(list((self.root / "env-cache").iterdir())), 4)
        self.assertEqual(len(list(home_cache.iterdir())), 4)

    def test_explicit_directory_beats_the_environment(self):
        with mock.patch.dict(os.environ, {"STANLI_BENCH_CACHE": str(self.root / "env-cache")}):
            self.run_main("--build-cache", str(self.root / "explicit"))
        self.assertFalse((self.root / "env-cache").exists())
        self.assertEqual(len(list((self.root / "explicit").iterdir())), 4)

    def test_no_build_cache_builds_in_the_run_directory(self):
        _, run = self.run_main("--no-build-cache")
        self.assertFalse((self.root / "home/.cache").exists())
        self.assertTrue((run / "a/gradbench").is_file())
        self.assertEqual([m[1]["cache"] for m in self.measured], [dict(directory=None, hit=False)] * 2)
        evidence = json.loads((run / "build-prepass.jsonl").read_text())
        self.assertIsNone(evidence["cache_directory"])

    def test_cache_flags_conflict_and_job_count_must_be_positive(self):
        for extra in (["--build-cache", "x", "--no-build-cache"], ["--build-jobs", "0"]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as raised:
                self.run_main(*extra)
            self.assertEqual(raised.exception.code, 2)

    def test_resume_builds_only_models_without_a_saved_result(self):
        self.run_main("--no-build-cache")
        run = pathlib.Path(str(self.root / "out.tsv") + ".run")
        (run / "b.result.json").unlink()
        self.measured.clear()
        self.run_main("--no-build-cache", "--resume")
        self.assertEqual([m[0] for m in self.measured], ["b"])
        phases = {call["phase"].split("/")[0] for call in self.runners[1].calls}
        self.assertEqual(phases, {"b"})


def mark_cached(record, run_id="older-run"):
    for key in ("cmdstan_stanc", "cmdstan_build"):
        record["setup"][key].update(cached=True, cache_key="k" * 64, cache_run_id=run_id)


class CachedSetupCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.paths = (root / "summary.tsv", root / "records.json.gz", root / "manifest.json")
        self.manifest, self.records, self.rows = artifact_fixture()

    def render(self):
        from tools.corpus_table import render_catalog
        write_artifacts(*self.paths, self.rows, self.records, self.manifest)
        return render_catalog(*self.paths)

    def test_cached_setup_events_are_accepted_and_the_intro_says_so(self):
        mark_cached(self.records[0])
        rendered = self.render()
        intro = rendered.split("| Model")[0]
        self.assertIn("CmdStan stanc and build times for 1 of 2 models were reused from the build cache", intro)
        self.assertIn("1.42x", rendered)
        self.assertIn("| 0.122 | 2.2 |", rendered)

    def test_intro_is_unchanged_when_nothing_was_reused(self):
        self.assertNotIn("build cache", self.render())

    def test_malformed_cached_events_are_rejected(self):
        changes = {"not true": dict(cached="yes"), "no key": dict(cache_key=""),
                   "no builder": dict(cache_run_id=None)}
        for label, change in changes.items():
            self.records[0]["setup"] = artifact_fixture()[1][0]["setup"]
            mark_cached(self.records[0])
            self.records[0]["setup"]["cmdstan_build"].update(change)
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "Invalid setup event"):
                self.render()

    def test_stanli_compile_is_never_reused(self):
        mark_cached(self.records[0])
        self.records[0]["setup"]["stanli_compile"].update(cached=True, cache_key="k", cache_run_id="r")
        with self.assertRaisesRegex(ValueError, "Invalid setup event"):
            self.render()

    def test_failed_event_cannot_be_cached(self):
        mark_cached(self.records[0])
        self.records[0]["setup"]["cmdstan_build"]["status"] = "failed"
        self.records[0]["row"]["cmdstan_build_s"] = ""
        self.rows[0]["cmdstan_build_s"] = ""
        with self.assertRaisesRegex(ValueError, "Invalid setup event"):
            self.render()

    def test_published_warm_run_keeps_the_cached_evidence_and_the_sentence(self):
        from tools.publish_corpus_bench import publish
        mark_cached(self.records[0])
        summary = pathlib.Path(self.temp.name) / "warm.tsv"
        run = pathlib.Path(str(summary) + ".run")
        run.mkdir()
        write_artifacts(summary, pathlib.Path(self.temp.name) / "unused.gz", run / "manifest.json",
                        self.rows, self.records, self.manifest)
        for record in self.records:
            (run / f"{record['model']}.result.json").write_text(json.dumps(record))
        (run / "events.jsonl").write_text("")
        with contextlib.redirect_stdout(io.StringIO()):
            publish(summary, pathlib.Path(self.temp.name) / "published")
        readme = (pathlib.Path(self.temp.name) / "published/README.md").read_text()
        self.assertIn("1 of 2 models were reused from the build cache", readme)
        import gzip
        published = json.loads(gzip.open(pathlib.Path(self.temp.name) / "published/model-results.json.gz", "rt").read())
        good = next(record for record in published if record["model"] == "good")
        self.assertTrue(good["setup"]["cmdstan_build"]["cached"])


class CachedSetupVectorizedTests(existing.VectorizedFixture):
    def test_intro_counts_models_whose_reused_cmdstan_build_came_from_the_cache(self):
        self.assertNotIn("build cache", self.render())
        mark_cached(self.sources["old"][1][0])
        intro = self.render().split("| Model")[0]
        self.assertIn("CmdStan stanc and build times for 1 of 2 models were reused from the build cache "
                      "and not remeasured in the runs that report them.", intro)


class VectorizedReuseIdentityTests(existing.VectorizedFixture):
    def test_driver_and_timer_sources_must_match_the_default_run(self):
        for path in ("tools/bench_cmdstan_grad.cpp", "tools/benchmark_timer.hpp"):
            self.sources["old"][0]["identity"]["sources"][path] = "edited"
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "sources"):
                self.render()
            self.sources["old"][0]["identity"]["sources"][path] = self.default[0]["identity"]["sources"][path]
        self.assertIn("| `good` | 4x |", self.render())

    def test_harness_and_stanli_side_sources_may_differ(self):
        sources = self.sources["old"][0]["identity"]["sources"]
        sources["harnesses/corpus_bench.py"] = "older-harness"
        sources["tools/bench_grad.cpp"] = "older-stanli-driver"
        self.assertIn("| `good` | 4x |", self.render())

    def test_missing_driver_identity_is_not_a_match(self):
        del self.sources["old"][0]["identity"]["sources"]["tools/benchmark_timer.hpp"]
        with self.assertRaisesRegex(ValueError, "sources"):
            self.render()

    def test_machine_must_match_the_default_run(self):
        for field, value in (("platform", "Linux-x86_64"), ("processor", "x86_64"), ("logical_cpus", 8)):
            machine = self.sources["old"][0]["identity"]["machine"]
            original = machine[field]
            machine[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "machine"):
                self.render()
            machine[field] = original

    def test_a_renamed_host_is_the_same_machine(self):
        self.sources["old"][0]["identity"]["machine"]["host"] = "Host.local"
        self.assertIn("| `good` | 4x |", self.render())


class CacheKeyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        (self.root / "cmdstan").mkdir()
        patch = mock.patch("harnesses.corpus_bench.compile_cmd", side_effect=fake_compile)
        patch.start()
        self.addCleanup(patch.stop)

    def key(self, args=None, expected=None, stan=STAN):
        context = cache_context(args or fake_args(self.root), expected or fake_expected())
        return corpus_bench.model_key(context, stan)

    def driver_key(self, args=None, expected=None, stan=STAN):
        context = cache_context(args or fake_args(self.root), expected or fake_expected())
        return corpus_bench.driver_key(context, stan)

    def test_key_is_a_stable_sha256(self):
        key = self.key()
        self.assertRegex(key, r"^[0-9a-f]{64}$")
        self.assertEqual(key, self.key())

    def test_every_component_of_the_build_changes_the_key(self):
        def edit(path, value):
            def apply(expected):
                target = expected
                for part in path[:-1]:
                    target = target[part]
                target[path[-1]] = value
            return apply
        changes = {
            "stanc binary": edit(("executables", "stanc"), "other"),
            "cmdstan checkout": edit(("toolchains", "cmdstan", "head"), "other"),
            "cmdstan diff": edit(("toolchains", "cmdstan", "tracked_diff_sha256"), "other"),
            "stan checkout": edit(("toolchains", "stan", "head"), "other"),
            "math checkout": edit(("toolchains", "math", "tracked_diff_sha256"), "other"),
            "libraries": edit(("toolchains", "libraries", "tbb/libtbb.dylib"), "other"),
            "clang": edit(("toolchains", "clang_sha256"), "other"),
            "build environment": edit(("toolchains", "build_environment", "CXXFLAGS"), "-O0"),
        }
        base = self.key()
        keys = {base}
        for label, change in changes.items():
            expected = fake_expected()
            change(expected)
            with self.subTest(label=label):
                key = self.key(expected=expected)
                self.assertNotEqual(key, base)
                keys.add(key)
        self.assertEqual(len(keys), len(changes) + 1)

    def test_driver_and_timer_sources_change_only_the_driver_key(self):
        base, base_driver = self.key(), self.driver_key()
        for label, source in (("driver", "tools/bench_cmdstan_grad.cpp"),
                              ("timer", "tools/benchmark_timer.hpp")):
            expected = fake_expected()
            expected["sources"][source] = "other"
            with self.subTest(label=label):
                self.assertEqual(self.key(expected=expected), base)
                self.assertNotEqual(self.driver_key(expected=expected), base_driver)

    def test_every_model_component_also_changes_the_driver_key(self):
        base = self.driver_key()
        expected = fake_expected()
        expected["executables"]["stanc"] = "other"
        self.assertNotEqual(self.driver_key(expected=expected), base)
        self.assertNotEqual(self.driver_key(stan="b" * 64), base)
        self.assertNotEqual(self.driver_key(), self.key())

    def test_source_flags_realpath_and_cache_version_change_the_key(self):
        base = self.key()
        self.assertNotEqual(self.key(stan="b" * 64), base)
        self.assertNotEqual(self.key(args=fake_args(self.root, stancflags="--O1")), base)
        other = self.root / "other-cmdstan"
        other.mkdir()
        self.assertNotEqual(self.key(args=fake_args(self.root, cmdstan=other)), base)
        with mock.patch("bench_build_cache.VERSION", 999):
            self.assertNotEqual(self.key(), base)

    def test_driver_command_changes_only_the_driver_key_and_make_changes_both(self):
        base, base_driver = self.key(), self.driver_key()
        with mock.patch("harnesses.corpus_bench.compile_cmd",
                        side_effect=lambda *a, **k: fake_compile(*a, **k) + ["-DNEW"]):
            self.assertEqual(self.key(), base)
            self.assertNotEqual(self.driver_key(), base_driver)
        with mock.patch("harnesses.corpus_bench.make_command",
                        side_effect=lambda *a, **k: ["make", "-j2", "{exe}"]):
            self.assertNotEqual(self.key(), base)

    def test_stanli_side_and_worktree_paths_do_not_change_the_key(self):
        base = self.key()
        expected = fake_expected()
        expected["executables"].update(bench="new-bench", vectorize_probe="new-probe")
        expected["sources"].update({"harnesses/corpus_bench.py": "edited",
                                    "tools/bench_grad.cpp": "edited"})
        expected["toolchains"]["stanli_math"]["head"] = "elsewhere"
        self.assertEqual(self.key(expected=expected), base)
        self.assertEqual(self.key(args=fake_args(self.root, stanc=self.root / "elsewhere/stanc")), base)
        with mock.patch("harnesses.corpus_bench.REPO", pathlib.Path("/another/worktree")):
            self.assertEqual(self.key(), base)

    @unittest.skipUnless(os.name == "posix", "symlinks need privileges on Windows")
    def test_symlinked_cmdstan_directory_keys_like_its_target(self):
        link = self.root / "link"
        link.symlink_to(self.root / "cmdstan")
        self.assertEqual(self.key(args=fake_args(self.root, cmdstan=link)), self.key())


class BuildCacheStoreTests(unittest.TestCase):
    def setUp(self):
        from bench_build_cache import BuildCache
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name) / "cache"
        self.cache = BuildCache(self.root)

    def build(self, key, text="built"):
        scratch = self.cache.scratch(key)
        (scratch / "model.stan").write_text(text)
        (scratch / "gradbench").write_text(text)
        return scratch

    def meta(self, **values):
        return dict(files=["model.stan", "gradbench"], **values)

    def test_commit_publishes_the_whole_directory_with_its_metadata(self):
        key = "k" * 64
        scratch = self.build(key)
        self.assertTrue(self.cache.commit(scratch, key, self.meta(run_id="r")))
        self.assertFalse(scratch.exists())
        self.assertEqual((self.root / key / "gradbench").read_text(), "built")
        self.assertEqual(self.cache.lookup(key)["run_id"], "r")
        self.assertEqual(self.cache.entry(key), self.root / key)
        self.assertEqual([p.name for p in self.root.iterdir()], [key])

    def test_missing_or_damaged_entries_are_misses(self):
        key = "k" * 64
        self.assertIsNone(self.cache.lookup(key))
        self.cache.commit(self.build(key), key, self.meta())
        (self.root / key / "gradbench").unlink()
        self.assertIsNone(self.cache.lookup(key))
        self.cache.entry(key).joinpath("meta.json").write_text("{")
        self.assertIsNone(self.cache.lookup(key))

    def test_losing_a_race_discards_our_directory_and_keeps_the_winner(self):
        key = "k" * 64
        self.assertTrue(self.cache.commit(self.build(key, "first"), key, self.meta(run_id="first")))
        loser = self.build(key, "second")
        self.assertFalse(self.cache.commit(loser, key, self.meta(run_id="second")))
        self.assertFalse(loser.exists())
        self.assertEqual((self.root / key / "gradbench").read_text(), "first")
        self.assertEqual(self.cache.lookup(key)["run_id"], "first")
        self.assertEqual([p.name for p in self.root.iterdir()], [key])

    def test_simultaneous_commits_leave_exactly_one_complete_entry(self):
        key = "k" * 64
        scratches = [self.build(key, f"writer {i}") for i in range(8)]
        barrier, wins = threading.Barrier(8), []
        def commit(index):
            barrier.wait()
            wins.append(self.cache.commit(scratches[index], key, self.meta(run_id=str(index))))
        threads = [threading.Thread(target=commit, args=(i,)) for i in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(sum(wins), 1)
        winner = self.cache.lookup(key)["run_id"]
        self.assertEqual((self.root / key / "gradbench").read_text(), f"writer {winner}")
        self.assertEqual([p.name for p in self.root.iterdir()], [key])

    def test_discard_removes_an_unfinished_build(self):
        scratch = self.build("k" * 64)
        self.cache.discard(scratch)
        self.assertFalse(scratch.exists())
        self.assertEqual(list(self.root.iterdir()), [])


class ConcurrentRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def test_parallel_commands_get_distinct_ids_and_whole_event_lines(self):
        runner = Runner(self.root)
        barrier, results = threading.Barrier(16), []
        def work(index):
            barrier.wait()
            results.append(runner.run(f"job/{index}", [sys.executable, "-c", f"print({index})"], 30))
        threads = [threading.Thread(target=work, args=(i,)) for i in range(16)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        events = [json.loads(line) for line in runner.events.read_text().splitlines()]
        self.assertEqual(sorted(event["id"] for event in events), list(range(16)))
        for event in events:
            self.assertEqual(event["status"], "ok")
            self.assertEqual(runner.text(event).strip(), event["phase"].split("/")[1])

    def test_resumed_runner_continues_after_the_highest_recorded_id(self):
        (self.root / "events.jsonl").write_text(
            "".join(json.dumps(dict(id=i)) + "\n" for i in (0, 2, 3)))
        self.assertEqual(Runner(self.root).next_id, 4)

    def test_kill_active_stops_running_commands_and_marks_them_interrupted(self):
        runner = Runner(self.root)
        started, outcome = threading.Event(), []
        def work():
            try:
                outcome.append(runner.run("slow", [sys.executable, "-c",
                    "import time; print('up', flush=True); time.sleep(60)"], 120))
            except BaseException as exc:
                outcome.append(exc)
        thread = threading.Thread(target=work)
        thread.start()
        deadline = threading.Event()
        for _ in range(200):
            if runner.active:
                break
            deadline.wait(0.05)
        runner.kill_active()
        thread.join(30)
        self.assertFalse(thread.is_alive())
        self.assertEqual(outcome[0]["status"], "failed")
        self.assertEqual(runner.active, set())


if __name__ == "__main__":
    unittest.main()
