#!/usr/bin/env python3
"""Arbitrary-precision accuracy check for the native matrix_exp and solve
backward rules against the nested-tape backward they replaced.

A ULP distance from the nested tape is only an acceptable outcome under the
project's reassociation rule when a higher-precision reference shows the new
rule at least as accurate; a long-double finite-difference oracle cannot
resolve that on a toolchain where long double equals double. This uses
mpmath at 60 decimal digits instead: for matrix_exp, the exact adjoint is
the upper-right block of expm([[A^T, G], [0, A^T]]); for the solve kernels,
it is the same closed-form each kernel now computes (A^-T G, masked to the
read triangle for TriLow), evaluated at 60 digits rather than double.

Random cases (fixed seed) are written to a text file, `hp_adjoint_dump`
(built from tools/hp_adjoint_dump.cpp) computes both the old nested-tape
adjoint and the new native adjoint at double precision for each case, and
this script compares both against the mpmath reference and reports ULP
distance, per element, max and median, old vs new.

Usage: tools/matrix_pullback_hp_check.py [--dump-bin PATH]
"""
import argparse
import pathlib
import random
import statistics
import struct
import subprocess
import sys
import tempfile

import mpmath

REPO = pathlib.Path(__file__).resolve().parent.parent
mpmath.mp.dps = 60

SEED = 20260918


def _key(d: float) -> int:
  i = struct.unpack("<q", struct.pack("<d", d))[0]
  return (-(1 << 63)) - i if i < 0 else i


def ulp(got: float, want_mp) -> int:
  # float(want_mp) is the correctly-rounded double nearest the 60-digit
  # value, so this is an ordinary ULP distance against that double.
  want = float(want_mp)
  return abs(_key(got) - _key(want))


def report(name: str, old_ulp, new_ulp):
  print(f"{name}: old max={max(old_ulp):.0f} median={statistics.median(old_ulp):.0f}"
        f"  new max={max(new_ulp):.0f} median={statistics.median(new_ulp):.0f}"
        f"  ({len(old_ulp)} elements)")


def matrix_exp_reference(a_mp, g_mp):
  n = a_mp.rows
  block = mpmath.zeros(2 * n)
  at = a_mp.T
  for i in range(n):
    for j in range(n):
      block[i, j] = at[i, j]
      block[n + i, n + j] = at[i, j]
      block[i, n + j] = g_mp[i, j]
  e = mpmath.expm(block)
  return e[0:n, n:2 * n]


def mat_solve(a_mp, b_mp):
  # mpmath.lu_solve only takes a vector right-hand side; solve column by
  # column for a matrix one.
  cols = [mpmath.lu_solve(a_mp, b_mp[:, j]) for j in range(b_mp.cols)]
  out = mpmath.zeros(a_mp.cols, b_mp.cols)
  for j, col in enumerate(cols):
    for i in range(a_mp.cols):
      out[i, j] = col[i]
  return out


def _mask_tri(m, n, lower):
  out = mpmath.zeros(n, m.cols)
  for i in range(n):
    js = range(0, i + 1) if lower else range(i, n)
    for j in js:
      if j < m.cols:
        out[i, j] = m[i, j]
  return out


def left_adjoint_reference(kind, a_mp, x_mp, g_mp, n, lower=True):
  # Mirrors matrix_fns.cpp's left_adjoint, at 60-digit precision.
  if kind == 2:
    tri = _mask_tri(a_mp, n, lower)
    y = mat_solve(tri.T, g_mp)
    adj_b = y
    adj_a = _mask_tri(-(y * x_mp.T), n, lower)
  elif kind == 1:
    y = mat_solve(a_mp, g_mp)  # SPD: a == a^T exactly
    adj_b = y
    adj_a = -(y * x_mp.T)
  else:
    y = mat_solve(a_mp.T, g_mp)
    adj_b = y
    adj_a = -(y * x_mp.T)
  return adj_a, adj_b


def solve_reference(kind, a_mp, x_mp, g_mp, n, left):
  if left:
    return left_adjoint_reference(kind, a_mp, x_mp, g_mp, n)
  # b / A mirrors matrix_fns.cpp's right_adjoint: a left solve on A', with
  # TriLow's masked triangle flipped, Spd's divisor adjoint read off
  # without transposing back.
  adj_a_t, adj_b_t = left_adjoint_reference(kind, a_mp.T, x_mp.T, g_mp.T, n,
                                            lower=(kind != 2))
  adj_a = adj_a_t if kind == 1 else adj_a_t.T
  adj_b = adj_b_t.T
  return adj_a, adj_b


def check_matrix_exp(dump_bin, rng):
  cases = []
  for n in (2, 5, 10, 20):
    for _ in range(2):
      a = [[rng.uniform(-0.5, 0.5) for _ in range(n)] for _ in range(n)]
      g = [[rng.uniform(-0.5, 0.5) for _ in range(n)] for _ in range(n)]
      cases.append((n, a, g))

  with tempfile.TemporaryDirectory() as td:
    case_file = pathlib.Path(td) / "cases.txt"
    with case_file.open("w") as f:
      f.write(f"{len(cases)}\n")
      for n, a, g in cases:
        f.write(f"matrix_exp {n}\n")
        for row in a:
          f.write(" ".join(repr(x) for x in row) + "\n")
        for row in g:
          f.write(" ".join(repr(x) for x in row) + "\n")
    out = subprocess.run([str(dump_bin), str(case_file)], capture_output=True,
                         text=True, check=True).stdout.split()

  pos = 0
  by_n = {}
  for n, a, g in cases:
    old = [float(x) for x in out[pos:pos + n * n]]
    pos += n * n
    new = [float(x) for x in out[pos:pos + n * n]]
    pos += n * n
    a_mp = mpmath.matrix([[mpmath.mpf(repr(x)) for x in row] for row in a])
    g_mp = mpmath.matrix([[mpmath.mpf(repr(x)) for x in row] for row in g])
    ref = matrix_exp_reference(a_mp, g_mp)
    old_u, new_u = by_n.setdefault(n, ([], []))
    for i in range(n):
      for j in range(n):
        old_u.append(ulp(old[i * n + j], ref[i, j]))
        new_u.append(ulp(new[i * n + j], ref[i, j]))

  for n in sorted(by_n):
    report(f"matrix_exp n={n}", *by_n[n])


def check_solve(dump_bin, rng):
  cases = []
  for left in (True, False):
    for kind in (0, 1, 2):
      for n in (5, 10):
        k = n
        br, bc = (n, k) if left else (k, n)
        for _ in range(2):
          base = [[rng.uniform(-0.2, 0.2) for _ in range(n)] for _ in range(n)]
          if kind == 1:
            a = [[base[i][j] + base[j][i] for j in range(n)] for i in range(n)]
            for i in range(n):
              a[i][i] += n + 1.0
          else:
            a = base
            for i in range(n):
              a[i][i] += n + 1.0
          b = [[rng.uniform(-0.3, 0.3) for _ in range(bc)] for _ in range(br)]
          cases.append((left, kind, n, k, br, bc, a, b))

  with tempfile.TemporaryDirectory() as td:
    case_file = pathlib.Path(td) / "cases.txt"
    with case_file.open("w") as f:
      f.write(f"{len(cases)}\n")
      for left, kind, n, k, br, bc, a, b in cases:
        f.write(f"solve {1 if left else 0} {kind} {n} {k}\n")
        for row in a:
          f.write(" ".join(repr(x) for x in row) + "\n")
        for row in b:
          f.write(" ".join(repr(x) for x in row) + "\n")
    out = subprocess.run([str(dump_bin), str(case_file)], capture_output=True,
                         text=True, check=True).stdout.split()

  pos = 0
  by_kind = {}
  names = {0: "plain", 1: "spd", 2: "trilow"}
  for left, kind, n, k, br, bc, a, b in cases:
    old_a = [float(x) for x in out[pos:pos + n * n]]; pos += n * n
    old_b = [float(x) for x in out[pos:pos + br * bc]]; pos += br * bc
    new_a = [float(x) for x in out[pos:pos + n * n]]; pos += n * n
    new_b = [float(x) for x in out[pos:pos + br * bc]]; pos += br * bc

    a_mp = mpmath.matrix([[mpmath.mpf(repr(a[i][j])) for j in range(n)] for i in range(n)])
    b_mp = mpmath.matrix([[mpmath.mpf(repr(b[i][j])) for j in range(bc)] for i in range(br)])
    if left:
      x_mp = mat_solve(a_mp if kind != 2 else _mask_tri(a_mp, n, True), b_mp)
    else:
      # x = b A^-1: solved as (x')= A'^-1 b' then transposed.
      at = a_mp.T if kind != 2 else _mask_tri(a_mp, n, True).T
      x_mp = mat_solve(at, b_mp.T).T
    g_mp = mpmath.ones(br, bc)
    adj_a_ref, adj_b_ref = solve_reference(kind, a_mp, x_mp, g_mp, n, left)

    key = (names[kind], "left" if left else "right", n)
    old_u, new_u = by_kind.setdefault(key, ([], []))
    for i in range(n):
      for j in range(n):
        old_u.append(ulp(old_a[i * n + j], adj_a_ref[i, j]))
        new_u.append(ulp(new_a[i * n + j], adj_a_ref[i, j]))
    for i in range(br):
      for j in range(bc):
        old_u.append(ulp(old_b[i * bc + j], adj_b_ref[i, j]))
        new_u.append(ulp(new_b[i * bc + j], adj_b_ref[i, j]))

  for key in sorted(by_kind):
    report(f"solve {key[0]:7s} {key[1]:5s} n={key[2]}", *by_kind[key])


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--dump-bin", default=str(REPO / "build-rel" / "hp_adjoint_dump"))
  args = ap.parse_args()
  dump_bin = pathlib.Path(args.dump_bin)
  if not dump_bin.exists():
    print(f"build it first: cmake --build build-rel --target hp_adjoint_dump",
          file=sys.stderr)
    return 2

  rng = random.Random(SEED)
  check_matrix_exp(dump_bin, rng)
  check_solve(dump_bin, rng)
  return 0


if __name__ == "__main__":
  sys.exit(main())
