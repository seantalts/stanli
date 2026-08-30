#!/usr/bin/env python3
"""Run one source-level Stanli lit test.

Each marked ``.stan`` case carries its expectation and optional JSON data in
comments:

    // STANLI-LIT: PASS
    // STANLI-LIT-EXPECT: OK
    // STANLI-LIT-DATA: {"N": 3}

Known gaps use ``XFAIL``.  Matching XFAILs pass the CTest invocation; a changed
result fails it so the case must be reviewed and promoted when the gap closes.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile


PREFIX = "// STANLI-LIT: "
EXPECT_PREFIX = "// STANLI-LIT-EXPECT: "
DATA_PREFIX = "// STANLI-LIT-DATA: "


def directive(lines: list[str], prefix: str) -> str | None:
    values = [line[len(prefix):].strip() for line in lines
              if line.startswith(prefix)]
    if len(values) > 1:
        raise ValueError(f"duplicate {prefix.strip()} directive")
    return values[0] if values else None


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
        try:
            completed = subprocess.run(
                [
                    str(args.stanli_check),
                    str(source),
                    str(data_path),
                ],
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
    if args.discover:
        print(f"{args.case}: {result}")
        return 0

    if matches(expect, result, completed.returncode):
        print(f"{status} {args.case}: {result}")
        return 0
    label = "FAIL" if status == "PASS" else "XPASS"
    print(f"{label} {args.case}: expected {expect!r}; got {result}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
