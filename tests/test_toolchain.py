#!/usr/bin/env python3
"""deps/pins.env is the one place the Stan toolchain is pinned."""

import pathlib
import re
import subprocess
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
