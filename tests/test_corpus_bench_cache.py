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
from harnesses.corpus_bench import cache_context

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


if __name__ == "__main__":
    unittest.main()
