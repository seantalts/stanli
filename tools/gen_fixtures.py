#!/usr/bin/env python3
"""Generate the legacy MIR test corpus from its checked-in Stan sources."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


REPO = pathlib.Path(__file__).resolve().parents[1]

# These are source-level lit corpora. The lit runner asks stanc for MIR in a
# temporary directory when each test runs, so also materializing their MIR in
# tests/fixtures only makes every ordinary build process a large duplicate
# corpus.
SOURCE_ONLY_PATTERN = re.compile(
    r"(?:builtin|density)_signatures_[0-9]+\.stan\Z")

# These models exercise structured control that O1 intentionally rewrites
# away. Keep the policy centralized here so CMake and the manual wrapper
# cannot generate different corpora.
O0_FIXTURES = {
    "call_argument_cache",
    "gq_udf_return_layout",
    "paramcond_intarray",
    "reduce_sum_arguments",
    "runtime_int_array_udf",
    "structured_matrix_ops",
    "udf_conditional_return",
    "udf_local_shape",
    "whileloop",
}


def fixture_sources(source_dir: pathlib.Path) -> list[pathlib.Path]:
    return [source for source in sorted(source_dir.glob("*.stan"))
            if SOURCE_ONLY_PATTERN.fullmatch(source.name) is None]


def canonical_mir(text: str, o0: bool) -> str:
    """Match the historical checked-in fixture spelling exactly."""
    if o0:
        text = "\n".join(line.rstrip(" \t") for line in text.splitlines())
    else:
        text = text.rstrip("\r\n")
    return text + "\n"


def generate(stanc: pathlib.Path, stan: pathlib.Path,
             output: pathlib.Path) -> None:
    stem = stan.stem
    relative_source = stan.relative_to(REPO).as_posix()
    flags = (["--debug-transformed-mir"] if stem in O0_FIXTURES else
             ["--O1", "--debug-optimized-mir"])
    completed = subprocess.run(
        [str(stanc), *flags, relative_source],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        check=False,
    )
    # stanc writes C++ beside the source even when MIR goes to stdout. It is
    # never an input to these tests and must not dirty the checkout.
    stan.with_suffix(".hpp").unlink(missing_ok=True)
    if completed.returncode:
        raise subprocess.CalledProcessError(completed.returncode,
                                            completed.args)

    text = canonical_mir(completed.stdout, stem in O0_FIXTURES)
    if output.exists() and output.read_text(encoding="utf-8") == text:
        # The build rule also depends on this script and the compiler. Refresh
        # an unchanged output so make/ninja does not rerun the whole corpus on
        # every build after either input changes.
        output.touch()
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stanc", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", default="tests/fixtures",
                        type=pathlib.Path)
    parser.add_argument("--output-dir", default="tests/fixtures",
                        type=pathlib.Path)
    parser.add_argument("--list-sources", action="store_true",
                        help="print the legacy fixture sources and exit")
    args = parser.parse_args()

    stanc = args.stanc.expanduser().resolve()
    source_dir = (REPO / args.source_dir).resolve()
    output_dir = (REPO / args.output_dir).resolve()
    if not stanc.is_file():
        parser.error(f"stanc executable not found: {stanc}")
    sources = fixture_sources(source_dir)
    if not sources:
        parser.error(f"no .stan fixtures found in {source_dir}")
    if args.list_sources:
        for source in sources:
            print(source)
        return 0

    expected = {f"{stan.stem}.tmir.sexp" for stan in sources}
    for stale in output_dir.glob("*.tmir.sexp"):
        if stale.name not in expected:
            stale.unlink()

    for stan in sources:
        generate(stanc, stan, output_dir / f"{stan.stem}.tmir.sexp")
    print(f"generated {len(sources)} MIR fixtures in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
