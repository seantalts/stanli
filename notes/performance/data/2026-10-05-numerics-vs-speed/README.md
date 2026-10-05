Data and scripts for [the numerics-versus-speed note](../../2026-10-05-numerics-vs-speed.md).

- `diagnostic-toggles.patch`: environment switches applied to f1face39 to undo
  each part of #403 separately (`X_NO_CSE_ACTIVE` restores merging of active
  duplicates; the reroll and partition switches allow fusion over shared
  parameters). Diagnostic only.
- `fusion-toggles.patch`: the same fusion switches ported to main 2222997f
  (`X_NO_RR_SHARED`, `X_NO_PART_SHARED`), used for the accuracy check.
- `hp_reference.py`: log density and gradients of s2_discrete_weibull and
  s2_hurdle_negbin in mpmath at 70 digits, at the corpus evaluation points.
- `compare_ulps.py`: compares CmdStan references, stanli, and stanli with the
  fusion switches against the reference. Run from a checkout with
  `fusion-toggles.patch` applied and `build-rel/stanli_check` built.
- `isa-timing-summary.txt`, `isa-variant-diff.tsv`: x86-64 baseline vs
  x86-64-v3 (with and without FMA) timing and numerics on a GitHub runner
  (AMD EPYC 7763), scratch branch `bench/avx2`.
