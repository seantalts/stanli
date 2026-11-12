#!/usr/bin/env python3
"""Check stanli's compile errors against stanc3's own expected output.

Every model in stanc3's test/integration/bad has its stanc output recorded in
a stanc.expected file. This runs each model through stanli_check the way
stanc3's test runner runs stanc (from the model's directory, with the bare
file name), and requires stanli's error text, exit status 1, and a
`COMPILE_FAIL <first line>` protocol line on stdout.

A model whose output differs must be listed in tests/stanc3_bad_ledger.json
with its cause and the first line stanli prints. An entry with `same_as`
names another model whose expected text, with the file name swapped, is what
stanli must print instead. A ledgered model that now matches fails as stale,
so the ledger can only shrink.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import difflib
import json
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import toolchain  # noqa: E402

EMPTY = REPO / "tests/function_coverage/empty.json"
LEDGER = REPO / "tests/stanc3_bad_ledger.json"
BLOCK = re.compile(r"^  \$ stanc (.*)$")
EXIT = re.compile(r"^\[exit \d+\]$")


def check_pin(src):
    if not (src / "test/integration/bad").is_dir():
        sys.exit(f"{src}/test/integration/bad is missing; run deps/fetch.sh")
    head = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"],
                          text=True, capture_output=True)
    want = toolchain.pins()["STANC3_SRC_SHA"]
    if head.returncode != 0 or head.stdout.strip() != want:
        sys.exit(f"{src} is at {head.stdout.strip() or '(not a checkout)'}, "
                 f"not the pinned {want}")


def parse_expected(path):
    """{file name as given to stanc: expected output lines}"""
    blocks = {}
    name = None
    for line in path.read_text().splitlines():
        match = BLOCK.match(line)
        if match:
            if name is not None:
                sys.exit(f"{path}: block '{name}' has no [exit N] line")
            name = match.group(1)
            if name in blocks:
                sys.exit(f"{path}: '{name}' appears twice")
            blocks[name] = []
            continue
        if name is None:
            sys.exit(f"{path}: text before the first '$ stanc' line")
        blocks[name].append(line)
        if EXIT.match(line):
            name = None
    if name is not None:
        sys.exit(f"{path}: block '{name}' has no [exit N] line")
    return blocks


def collect(bad):
    """[(relative name, directory, file name, expected lines)]"""
    cases = []
    unmatched = []
    for directory in sorted({p.parent for p in bad.rglob("*")
                             if p.is_file() and p.suffix in
                             (".stan", ".stanfunctions")}
                            | {p.parent for p in bad.rglob("stanc.expected")}):
        expected_file = directory / "stanc.expected"
        on_disk = {p.name for p in directory.iterdir()
                   if p.is_file() and p.suffix in (".stan", ".stanfunctions")}
        blocks = parse_expected(expected_file) if expected_file.exists() \
            else {}
        rel = directory.relative_to(bad).as_posix()
        for name in sorted(on_disk - blocks.keys()):
            unmatched.append(f"{rel}/{name}: no stanc.expected block")
        for name in sorted(blocks.keys() - on_disk):
            unmatched.append(f"{rel}/{name}: expected block, no such file")
        for name in sorted(on_disk & blocks.keys()):
            label = name if rel == "." else f"{rel}/{name}"
            cases.append((label, directory, name, blocks[name]))
    if unmatched:
        sys.exit("expected blocks and model files do not match:\n  " +
                 "\n  ".join(unmatched))
    return cases


def run(case, check):
    label, directory, name, _ = case
    proc = subprocess.run([str(check), name, str(EMPTY)], cwd=directory,
                          text=True, capture_output=True, timeout=300)
    lines = proc.stderr.splitlines() + [f"[exit {proc.returncode}]"]
    protocol = None
    if proc.returncode != 0:
        first = lines[0] if len(lines) > 1 else ""
        if proc.stdout != f"COMPILE_FAIL {first}\n":
            protocol = proc.stdout
    return label, proc.returncode, lines, protocol


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", type=pathlib.Path, required=True)
    ap.add_argument("--stanc3-src", type=pathlib.Path,
                    default=REPO / "deps/stanc3-src")
    ap.add_argument("--ledger", type=pathlib.Path, default=LEDGER)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    check_pin(args.stanc3_src)
    bad = args.stanc3_src / "test/integration/bad"
    cases = collect(bad)
    expected = {label: lines for label, _, _, lines in cases}
    names = {label: name for label, _, name, _ in cases}
    ledger = json.loads(args.ledger.read_text())
    unknown = sorted(ledger.keys() - expected.keys())
    if unknown:
        sys.exit("ledger names models that do not exist: " + ", ".join(unknown))

    check = args.build.resolve() / "stanli_check"
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda c: run(c, check), cases))

    failures = []
    exact = 0
    compiled = []
    for label, status, got, protocol in results:
        want = expected[label]
        entry = ledger.get(label)
        if status == 0:
            compiled.append(label)
        if protocol is not None:
            failures.append(f"{label}: stdout is {protocol!r}, not "
                            f"'COMPILE_FAIL <first error line>'")
        if got == want:
            if entry:
                failures.append(f"{label}: matches stanc now; remove its "
                                "stale ledger entry")
            exact += 1
            continue
        if not entry:
            diff = "\n".join(difflib.unified_diff(
                want, got, "stanc", "stanli", lineterm=""))
            failures.append(f"{label}: differs from stanc and is not "
                            f"ledgered\n{diff}")
            continue
        if "same_as" in entry:
            other = entry["same_as"]
            if other not in expected:
                failures.append(f"{label}: same_as names unknown {other}")
                continue
            swapped = [line.replace(names[other], names[label])
                       for line in expected[other]]
            if got != swapped:
                failures.append(f"{label}: no longer prints {other}'s "
                                "stanc output with its own file name")
        elif got[0] != entry["stanli"]:
            failures.append(f"{label}: ledgered first line was "
                            f"{entry['stanli']!r}, now {got[0]!r}")

    causes = {}
    for label, entry in ledger.items():
        causes.setdefault(entry["cause"], []).append(label)
    print(f"{len(cases)} stanc3 bad models: {exact} match stanc exactly, "
          f"{len(ledger)} ledgered")
    for cause, labels in sorted(causes.items()):
        print(f"  {len(labels)}: {cause}")
    if compiled:
        print("stanli compiled models stanc rejects: " + ", ".join(compiled))
    if failures:
        print("\n".join(failures[:40]), file=sys.stderr)
        if len(failures) > 40:
            print(f"... and {len(failures) - 40} more", file=sys.stderr)
        print(f"FAIL: {len(failures)} models", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
