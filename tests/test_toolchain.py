#!/usr/bin/env python3
"""deps/pins.env is the one place the Stan toolchain is pinned."""

import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import toolchain

KEYS = ("CMDSTAN_TAG", "CMDSTAN_SHA", "STAN_SHA", "MATH_SHA", "MATH_VERSION",
        "STANC3_SRC_REPO", "STANC3_SRC_SHA", "STANC3_VERSION", "BRIDGESTAN_SHA",
        "PDB_SHA", "FMT_SHA", "OCAML_VERSION", "OPAM_SWITCH")
SHAS = ("CMDSTAN_SHA", "STAN_SHA", "MATH_SHA", "STANC3_SRC_SHA",
        "BRIDGESTAN_SHA", "PDB_SHA", "FMT_SHA")


class PinsFileTests(unittest.TestCase):
    def test_every_key_is_present_and_nothing_else(self):
        self.assertEqual(set(toolchain.pins()), set(KEYS))

    def test_shas_are_full_hex(self):
        pins = toolchain.pins()
        for key in SHAS:
            with self.subTest(key=key):
                self.assertRegex(pins[key], r"^[0-9a-f]{40}$")

    def test_release_labels_have_the_expected_shape(self):
        pins = toolchain.pins()
        self.assertRegex(pins["CMDSTAN_TAG"], r"^v\d+\.\d+\.\d+$")
        self.assertRegex(pins["MATH_VERSION"], r"^\d+\.\d+\.\d+$")
        self.assertRegex(pins["STANC3_VERSION"], r"^\d+\.\d+\.\d+$")
        self.assertEqual(toolchain.cmdstan_version(), pins["CMDSTAN_TAG"][1:])

    def test_the_file_is_sourceable_by_bash(self):
        out = subprocess.run(
            ["bash", "-c", 'set -eu; . "$1"; echo "$CMDSTAN_SHA $OPAM_SWITCH"',
             "bash", str(toolchain.PINS_FILE)],
            capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual(out, [toolchain.pins()["CMDSTAN_SHA"], toolchain.pins()["OPAM_SWITCH"]])

    def test_comments_and_blank_lines_are_skipped(self):
        parsed = toolchain.parse_pins("# note\n\nA=1\nB=https://x/y.git\n")
        self.assertEqual(parsed, {"A": "1", "B": "https://x/y.git"})

    def test_a_malformed_line_is_an_error(self):
        with self.assertRaises(ValueError):
            toolchain.parse_pins("A=1\nnot a pin\n")


class UpstreamAgreementTests(unittest.TestCase):
    def setUp(self):
        self.pins = toolchain.pins()
        self.checkout = REPO / "deps" / "cmdstan"
        try:
            head = toolchain.git_head(self.checkout)
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("no CmdStan checkout")
        if head != self.pins["CMDSTAN_SHA"]:
            self.skipTest("deps/cmdstan is not at CMDSTAN_SHA")

    def pointer(self, repo, path, rev):
        line = subprocess.check_output(["git", "-C", str(repo), "ls-tree", rev, path],
                                       text=True)
        return line.split()[2]

    def test_stan_and_math_are_the_submodules_cmdstan_pins(self):
        stan = self.pointer(self.checkout, "stan", self.pins["CMDSTAN_SHA"])
        self.assertEqual(stan, self.pins["STAN_SHA"])
        math = self.pointer(self.checkout / "stan", "lib/stan_math", stan)
        self.assertEqual(math, self.pins["MATH_SHA"])


def git(path, *args):
    return subprocess.run(
        ["git", "-C", str(path), "-c", "user.name=t", "-c", "user.email=t@example.com", *args],
        check=True, capture_output=True, text=True).stdout.strip()


def commit_repo(path, files):
    path.mkdir(parents=True, exist_ok=True)
    git(path, "init", "-q")
    for name, content in files.items():
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_text(content)
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", "x")
    return git(path, "rev-parse", "HEAD")


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.cmdstan = root / "cmdstan"
        self.heads = dict(
            cmdstan=commit_repo(self.cmdstan, {"makefile": "all:\n", "make/local": "# local\n"}),
            stan=commit_repo(self.cmdstan / "stan", {"a": "1"}),
            math=commit_repo(self.cmdstan / "stan/lib/stan_math", {"b": "2"}))
        self.pinned = root / "stanc-pinned"
        self.pinned.write_bytes(b"pinned stanc")
        self.pinned.chmod(0o755)
        pathlib.Path(str(self.pinned) + ".src").write_text("s" * 40 + "\n")
        (self.cmdstan / "bin").mkdir()
        (self.cmdstan / "bin/stanc").write_bytes(b"pinned stanc")
        pins = {"CMDSTAN_SHA": self.heads["cmdstan"], "STAN_SHA": self.heads["stan"],
                "MATH_SHA": self.heads["math"], "STANC3_SRC_SHA": "s" * 40}
        for patcher in (mock.patch.object(toolchain, "pins", lambda: pins),
                        mock.patch.object(toolchain, "PINNED_STANC", self.pinned)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def checks(self, **kwargs):
        found = toolchain.require_pinned_toolchain(self.cmdstan, allow_unpinned=True, **kwargs)
        return [m["check"] for m in found["mismatches"]]

    def test_a_pinned_toolchain_passes_and_records_the_stanc(self):
        found = toolchain.require_pinned_toolchain(self.cmdstan)
        self.assertEqual(found["mismatches"], [])
        self.assertEqual(found["stanc_sha256"], hashlib.sha256(b"pinned stanc").hexdigest())

    def test_untracked_files_and_make_local_edits_are_allowed(self):
        (self.cmdstan / "make/local").write_text("CXX=clang++\n")
        (self.cmdstan / "stray.txt").write_text("x")
        self.assertEqual(self.checks(), [])

    def test_a_modified_tracked_file_is_refused_with_the_fix(self):
        (self.cmdstan / "makefile").write_text("all:\n\t@true\n")
        with self.assertRaises(ValueError) as raised:
            toolchain.require_pinned_toolchain(self.cmdstan)
        message = str(raised.exception)
        self.assertIn("makefile", message)
        self.assertIn("fetch_cmdstan.sh", message)
        self.assertEqual(self.checks(), ["cmdstan_tree"])

    def test_another_cmdstan_head_names_both_shas(self):
        git(self.cmdstan, "commit", "-q", "--allow-empty", "-m", "y")
        new = git(self.cmdstan, "rev-parse", "HEAD")
        with self.assertRaises(ValueError) as raised:
            toolchain.require_pinned_toolchain(self.cmdstan)
        message = str(raised.exception)
        self.assertIn(self.heads["cmdstan"], message)
        self.assertIn(new, message)
        self.assertIn("harnesses/conformance/fetch_cmdstan.sh", message)
        found = toolchain.require_pinned_toolchain(self.cmdstan, allow_unpinned=True)
        self.assertEqual(found["mismatches"],
                         [dict(check="cmdstan_head", expected=self.heads["cmdstan"], found=new)])

    def test_stan_and_math_heads_are_checked(self):
        git(self.cmdstan / "stan", "commit", "-q", "--allow-empty", "-m", "y")
        git(self.cmdstan / "stan/lib/stan_math", "commit", "-q", "--allow-empty", "-m", "y")
        self.assertEqual(self.checks(), ["stan_head", "math_head"])

    def test_a_different_default_stanc_is_refused(self):
        (self.cmdstan / "bin/stanc").write_bytes(b"nightly")
        with self.assertRaises(ValueError) as raised:
            toolchain.require_pinned_toolchain(self.cmdstan)
        self.assertIn("bin/stanc", str(raised.exception))
        self.assertEqual(self.checks(), ["cmdstan_stanc"])

    def test_an_explicit_stanc_must_be_the_pinned_one(self):
        other = pathlib.Path(self.temp.name) / "patched"
        other.write_bytes(b"patched stanc")
        self.assertEqual(self.checks(stanc_path=other), ["stanc"])
        same = pathlib.Path(self.temp.name) / "copy"
        same.write_bytes(b"pinned stanc")
        self.assertEqual(self.checks(stanc_path=same), [])

    def test_a_declared_stanc_is_accepted_and_its_sha_recorded(self):
        other = pathlib.Path(self.temp.name) / "patched"
        other.write_bytes(b"patched stanc")
        found = toolchain.require_pinned_toolchain(self.cmdstan, stanc_path=other,
                                                   stanc_declared=True)
        self.assertEqual(found["mismatches"], [])
        self.assertEqual(found["stanc_sha256"], hashlib.sha256(b"patched stanc").hexdigest())

    def test_a_declared_stanc_does_not_excuse_the_installed_one(self):
        (self.cmdstan / "bin/stanc").write_bytes(b"nightly")
        other = pathlib.Path(self.temp.name) / "patched"
        other.write_bytes(b"patched stanc")
        self.assertEqual(self.checks(stanc_path=other, stanc_declared=True), ["cmdstan_stanc"])

    def test_a_pinned_stanc_built_from_another_revision_is_refused(self):
        pathlib.Path(str(self.pinned) + ".src").write_text("t" * 40 + "\n")
        self.assertEqual(self.checks(), ["pinned_stanc_provenance"])

    def test_a_directory_that_is_not_a_checkout_is_refused_even_when_unpinned_is_allowed(self):
        with tempfile.TemporaryDirectory() as plain:
            with self.assertRaises(ValueError) as raised:
                toolchain.require_pinned_toolchain(pathlib.Path(plain), allow_unpinned=True)
        self.assertIn("not a CmdStan git checkout", str(raised.exception))


class BuildStampTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.cmdstan = root / "cmdstan"
        commit_repo(self.cmdstan, {"makefile": "all:\n"})
        commit_repo(self.cmdstan / "stan", {"a": "1"})
        commit_repo(self.cmdstan / "stan/lib/stan_math", {"b": "2"})
        (self.cmdstan / "bin").mkdir()
        (self.cmdstan / "bin/stanc").write_bytes(b"stanc one")
        patcher = mock.patch.object(toolchain, "compiler_identity", lambda cmdstan: "clang 1")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_the_stamp_names_the_three_heads_the_stanc_and_the_compiler(self):
        stamp = toolchain.build_stamp(self.cmdstan)
        for head in (toolchain.git_head(self.cmdstan),
                     toolchain.git_head(self.cmdstan / "stan"),
                     toolchain.git_head(self.cmdstan / "stan/lib/stan_math"),
                     hashlib.sha256(b"stanc one").hexdigest(), "clang 1"):
            self.assertIn(head, stamp)

    def test_the_stamp_is_stable_and_moves_with_every_input(self):
        stamp = toolchain.build_stamp(self.cmdstan)
        self.assertEqual(toolchain.build_stamp(self.cmdstan), stamp)
        (self.cmdstan / "bin/stanc").write_bytes(b"stanc two")
        self.assertNotEqual(toolchain.build_stamp(self.cmdstan), stamp)
        (self.cmdstan / "bin/stanc").write_bytes(b"stanc one")
        git(self.cmdstan / "stan/lib/stan_math", "commit", "-q", "--allow-empty", "-m", "y")
        self.assertNotEqual(toolchain.build_stamp(self.cmdstan), stamp)
        git(self.cmdstan / "stan/lib/stan_math", "reset", "-q", "--hard", "HEAD~1")
        self.assertEqual(toolchain.build_stamp(self.cmdstan), stamp)
        with mock.patch.object(toolchain, "compiler_identity", lambda cmdstan: "clang 2"):
            self.assertNotEqual(toolchain.build_stamp(self.cmdstan), stamp)

    def test_a_missing_or_different_stamp_file_is_stale(self):
        self.assertFalse(toolchain.stamp_is_current(self.cmdstan))
        (self.cmdstan / toolchain.STAMP_NAME).write_text(toolchain.build_stamp(self.cmdstan))
        self.assertTrue(toolchain.stamp_is_current(self.cmdstan))
        (self.cmdstan / "bin/stanc").write_bytes(b"stanc two")
        self.assertFalse(toolchain.stamp_is_current(self.cmdstan))


if __name__ == "__main__":
    unittest.main()
