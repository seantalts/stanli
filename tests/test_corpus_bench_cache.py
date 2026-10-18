#!/usr/bin/env python3
"""Focused tests for the corpus benchmark's CmdStan build cache and build pre-pass."""

import copy
import json
import os
import pathlib
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from harnesses import corpus_bench
from harnesses.corpus_bench import Runner, cache_context

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
        from bench_build_cache import cache_key
        context = cache_context(args or fake_args(self.root), expected or fake_expected())
        return cache_key(context, stan)

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
            "driver source": edit(("sources", "tools/bench_cmdstan_grad.cpp"), "other"),
            "timer header": edit(("sources", "tools/benchmark_timer.hpp"), "other"),
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

    def test_source_flags_realpath_and_cache_version_change_the_key(self):
        base = self.key()
        self.assertNotEqual(self.key(stan="b" * 64), base)
        self.assertNotEqual(self.key(args=fake_args(self.root, stancflags="--O1")), base)
        other = self.root / "other-cmdstan"
        other.mkdir()
        self.assertNotEqual(self.key(args=fake_args(self.root, cmdstan=other)), base)
        with mock.patch("bench_build_cache.VERSION", 999):
            self.assertNotEqual(self.key(), base)

    def test_driver_command_and_make_invocation_change_the_key(self):
        base = self.key()
        with mock.patch("harnesses.corpus_bench.compile_cmd",
                        side_effect=lambda *a, **k: fake_compile(*a, **k) + ["-DNEW"]):
            self.assertNotEqual(self.key(), base)
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
