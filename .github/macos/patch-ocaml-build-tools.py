#!/usr/bin/env python3
"""Repair diagnostics in the source of macOS OCaml build-tool dependencies.

An opam pre-build hook runs in each package's unpacked source directory.
Keep the expected versions explicit and fail if an upstream source changes
instead of silently skipping a repair. These packages support Dune/PPX tools;
they are not runtime dependencies of the shipped stanc executables.
"""

from pathlib import Path
import sys


PATCHES = {
    # Backport ocaml-dune/csexp@07eb8988452ad51a09d0ab7379d73a87674aba6e:
    # "inlined" annotates a call; this is a function definition.
    "csexp": (
        "1.5.2", "src/csexp.ml",
        "    [@@inlined always]", "    [@@inline always]",
    ),
    # This higher-order argument has no known implementation in the generic
    # function. Keep the existing outer-function inlining and callers intact.
    "base": (
        "v0.17.3", "src/array.ml",
        "((keep_left_if [@inlined]) (compare x !result))",
        "(keep_left_if (compare x !result))",
    ),
    # Clang also defines __GNUC__, but does not implement GCC's optimize
    # attribute. Preserve the ordinary comparison and the GCC specialization.
    "ocaml_intrinsics_kernel": (
        "v0.17.3", "src/float_stubs.c",
        '#if defined(__GNUC__)\n__attribute__((optimize("no-math-errno")))',
        '#if defined(__GNUC__) && !defined(__clang__)\n'
        '__attribute__((optimize("no-math-errno")))',
    ),
}


def main():
    name, version = sys.argv[1:]
    if name not in PATCHES:
        return
    expected, filename, before, after = PATCHES[name]
    if version != expected:
        raise SystemExit(f"Review the macOS build-tool repair for {name}.{version}; "
                         f"expected {expected}")
    path = Path(filename)
    source = path.read_text()
    if source.count(before) == 1 and after not in source:
        path.write_text(source.replace(before, after))
        print(f"Repaired {name}.{version}: {filename}")
    elif before not in source and source.count(after) == 1:
        print(f"Already repaired {name}.{version}: {filename}")
    else:
        raise SystemExit(f"Unexpected source for {name}.{version}: {filename}")


if __name__ == "__main__":
    main()
