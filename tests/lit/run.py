#!/usr/bin/env python3
"""Run one source-level Stanli lit test.

Each marked ``.stan`` case carries its expectation and optional JSON data in
comments:

    // STANLI-LIT: PASS
    // STANLI-LIT-EXPECT: OK
    // STANLI-LIT-DATA: {"N": 3}

Known gaps use ``XFAIL``.  Matching XFAILs pass the CTest invocation; a changed
result fails it so the case must be reviewed and promoted when the gap closes.

A case can also assert the shape of one lowering-pass dump:

    // STANLI-LIT-DUMP: log_prob:reroll
    // STANLI-LIT-CHECK: s{{[0-9]+}}[195] = FMA
    // STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[1] = INDEX
    // STANLI-LIT-CHECK-NOT: SUM_VEC
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import tempfile


PREFIX = "// STANLI-LIT: "
EXPECT_PREFIX = "// STANLI-LIT-EXPECT: "
DATA_PREFIX = "// STANLI-LIT-DATA: "
DUMP_PREFIX = "// STANLI-LIT-DUMP: "
CHECK_PREFIXES = {
    "// STANLI-LIT-CHECK: ": "CHECK",
    "// STANLI-LIT-CHECK-NEXT: ": "CHECK-NEXT",
    "// STANLI-LIT-CHECK-NOT: ": "CHECK-NOT",
}


def directive(lines: list[str], prefix: str) -> str | None:
    values = [line[len(prefix):].strip() for line in lines
              if line.startswith(prefix)]
    if len(values) > 1:
        raise ValueError(f"duplicate {prefix.strip()} directive")
    return values[0] if values else None


def checks(lines: list[str]) -> list[tuple[str, str]]:
    found = []
    for line in lines:
        for prefix, kind in CHECK_PREFIXES.items():
            if line.startswith(prefix):
                found.append((kind, line[len(prefix):].strip()))
    return found


def compile_pattern(pattern: str) -> re.Pattern[str]:
    parts = re.split(r"\{\{(.*?)\}\}", pattern)
    return re.compile("".join(part if i % 2 else re.escape(part)
                              for i, part in enumerate(parts)))


def slice_region(stdout: str, stage: str) -> list[str] | None:
    begin, end = f";; {stage}", f";; end {stage}"
    lines = stdout.splitlines()
    try:
        first = lines.index(begin)
        last = lines.index(end, first)
    except ValueError:
        return None
    return lines[first + 1:last]


def run_checks(region: list[str],
               directives: list[tuple[str, str]]) -> str | None:
    """Return a failure message, or None when every directive is satisfied."""
    pos = 0
    pending_not = []
    for kind, pattern in directives:
        rx = compile_pattern(pattern)
        if kind == "CHECK-NOT":
            pending_not.append((pattern, rx))
            continue
        end = pos + 1 if kind == "CHECK-NEXT" else len(region)
        hit = next((i for i in range(pos, min(end, len(region)))
                    if rx.search(region[i])), None)
        if hit is None:
            return f"{kind}: {pattern}"
        for not_pattern, not_rx in pending_not:
            if any(not_rx.search(line) for line in region[pos:hit]):
                return f"CHECK-NOT: {not_pattern}"
        pending_not = []
        pos = hit + 1
    for not_pattern, not_rx in pending_not:
        if any(not_rx.search(line) for line in region[pos:]):
            return f"CHECK-NOT: {not_pattern}"
    return None


def result_line(completed: subprocess.CompletedProcess[str]) -> str:
    for line in reversed(completed.stdout.splitlines()):
        if line.startswith(("OK ", "COMPILE_FAIL ", "EVAL_FAIL ")):
            return line
    if completed.returncode < 0:
        return f"CRASH signal={-completed.returncode}"
    return f"NO_RESULT returncode={completed.returncode}"


def matches(expect: str, result: str, returncode: int) -> bool:
    if expect == "OK":
        return returncode == 0 and result.startswith("OK ")
    if expect == "CRASH":
        # POSIX reports a signal as a negative code; Windows reports an
        # exception status as a nonzero unsigned-looking value. In both cases
        # a crash has no stanli_check result line. A future checked rejection
        # produces EVAL_FAIL and deliberately stops matching this XFAIL.
        return returncode != 0 and result.startswith(("CRASH ", "NO_RESULT "))
    return returncode != 0 and expect in result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", type=pathlib.Path)
    parser.add_argument("stanli_check", type=pathlib.Path)
    parser.add_argument("--discover", action="store_true")
    args = parser.parse_args()

    try:
        source_text = args.case.read_text(encoding="utf-8")
        lines = source_text.splitlines()
        status = directive(lines, PREFIX)
        expect = directive(lines, EXPECT_PREFIX)
        data_text = directive(lines, DATA_PREFIX) or "{}"
        dump_stage = directive(lines, DUMP_PREFIX)
        check_directives = checks(lines)
        if check_directives and not dump_stage:
            raise ValueError("CHECK directives need a STANLI-LIT-DUMP stage")
        if status not in {"PASS", "XFAIL"}:
            raise ValueError("STANLI-LIT must be PASS or XFAIL")
        if not expect:
            raise ValueError("missing STANLI-LIT-EXPECT directive")
        data = json.loads(data_text)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"FAIL {args.case}: {error}")
        return 1

    if not args.stanli_check.is_file():
        print(f"FAIL missing stanli_check executable: {args.stanli_check}")
        return 1

    # stanc writes a sibling .hpp even when MIR goes to stdout.  Compile a
    # temporary copy so source-only lit runs never dirty the checkout.
    with tempfile.TemporaryDirectory(prefix="stanli-lit-") as tmp:
        root = pathlib.Path(tmp)
        source = root / args.case.name
        data_path = root / f"{args.case.stem}.json"
        source.write_text(source_text, encoding="utf-8")
        data_path.write_text(json.dumps(data), encoding="utf-8")
        command = [str(args.stanli_check), str(source), str(data_path)]
        if dump_stage:
            command.append(f"--dump-passes={dump_stage}")
        try:
            completed = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            print(f"FAIL {args.case}: timed out after 30 seconds")
            return 1

    result = result_line(completed)
    region = slice_region(completed.stdout, dump_stage) if dump_stage else None

    if args.discover:
        if dump_stage:
            if region is None:
                print(f"{args.case}: no ';; {dump_stage}' region in:")
                print(completed.stdout)
            else:
                print("\n".join(region))
        print(f"{args.case}: {result}")
        return 0

    if dump_stage and region is None:
        print(f"FAIL {args.case}: no ';; {dump_stage}' region in stdout")
        print(completed.stdout)
        return 1

    if check_directives:
        failed = run_checks(region, check_directives)
        if failed:
            print(f"FAIL {args.case}: unmatched {failed}")
            print("\n".join(region))
            return 1

    if matches(expect, result, completed.returncode):
        print(f"{status} {args.case}: {result}")
        return 0
    label = "FAIL" if status == "PASS" else "XPASS"
    print(f"{label} {args.case}: expected {expect!r}; got {result}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
