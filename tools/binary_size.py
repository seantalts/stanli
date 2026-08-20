#!/usr/bin/env python3
"""Attribute the shipped library's size by demangled symbol.

The README's binary-size table used to be hand-made, which meant it went
stale the moment a kernel was added -- and silently, because nothing
recomputes it. This does, so it can be regenerated with the artifact it
describes.

It has to run BEFORE tools/build_wheel.sh strips the library: the shipped
.so exports 762 symbols and carries no static symbol table, so the table
cannot be rebuilt from anything a user or CI ever downloads.

  tools/binary_size.py LIB [--markdown]

Attribution is by symbol name, first match wins, so the order of RULES is
the definition of each bucket. Symbols the rules do not claim land in
"unattributed" rather than being spread over the others.
"""
import collections
import pathlib
import re
import subprocess
import sys

# First match wins. Ordered most specific to least: a density is also a
# stan::math symbol, and Eigen appears inside both.
RULES = (
    ("embedded stanc3 (all OCaml)", re.compile(r"^caml|^camlStanc|_ocaml")),
    ("densities and distribution functions",
     re.compile(r"_lpdf|_lpmf|_cdf|_lcdf|_lccdf|_rng\b|stanli::.*densit")),
    ("SUNDIALS", re.compile(r"^_?(N_V|CV|IDA|SUNLin|SUNMat|SUNNonlin|ARK)")),
    ("Eigen (out-of-line)", re.compile(r"Eigen::")),
    ("Boost, nlohmann/json, NUTS, libc\\+\\+",
     re.compile(r"boost::|nlohmann::|stanli::(nuts|walnuts)|^_?std::|__cxx")),
    ("stan-math, everything else", re.compile(r"stan::math::|stan::")),
    ("stanli itself", re.compile(r"stanli::")),
)


def symbol_sizes(lib):
    """(size, demangled name) for every sized symbol in the object."""
    out = subprocess.run(["nm", "--print-size", "--demangle", str(lib)],
                         capture_output=True, text=True)
    if out.returncode or not out.stdout.strip():
        # Apple's nm spells the long options differently.
        out = subprocess.run(["nm", "-S", "-C", str(lib)],
                             capture_output=True, text=True)
    rows = []
    for line in out.stdout.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) < 4:
            continue
        try:
            size = int(parts[1], 16)
        except ValueError:
            continue
        if size:
            rows.append((size, parts[3]))
    return rows


def attribute(rows):
    buckets = collections.Counter()
    for size, name in rows:
        for label, pattern in RULES:
            if pattern.search(name):
                buckets[label] += size
                break
        else:
            buckets["unattributed"] += size
    return buckets


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    lib = pathlib.Path(argv[0])
    rows = symbol_sizes(lib)
    if not rows:
        raise SystemExit(
            f"{lib}: no sized symbols. Already stripped? This has to run "
            "before tools/build_wheel.sh strips the library.")
    buckets = attribute(rows)
    total = sum(buckets.values())
    on_disk = lib.stat().st_size
    lines = [f"Attributed {total / 1048576:.2f} MB of symbols in a "
             f"{on_disk / 1048576:.2f} MB library.", "",
             "| | | |", "| --- | ---: | ---: |"]
    for label, size in buckets.most_common():
        lines.append(f"| {label} | {size / 1048576:.2f} MB | "
                     f"{100.0 * size / total:.1f}% |")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
