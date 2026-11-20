# Where stanli is less accurate than CmdStan (audit of 2026-11-20)

Measured on tree `f94c05155`, Darwin arm64, against the 80-digit values of
`tools/hp`. The full report, with every command, is
`8-accuracy.md` of the 2026-11-20 audit. Nothing in `runtime/` was changed.

## Result

Of the `main` entries of `tests/ulp_ledger.json`, 27 were farther from the true
value than CmdStan. 25 of them come from the merge that re-roll (or partition,
when re-roll is off) makes of a loop of per-observation calls. With
`STANLI_NO_REROLL=1 STANLI_NO_PARTITION=1`, 23 of the 25 return CmdStan's value
bit for bit and the other 2 become closer to the truth than CmdStan. The merged
kernels add thousands of terms in a plain running sum, in a different order
from CmdStan's tape, and both orders lose up to about 220 scaled ULP. A Python
reimplementation of the `ordered_logistic` cut-point gradient of `ch12_m12_5`
matches stanli and CmdStan bit for bit at all three points; with a compensated
(Neumaier) sum in its place the gradient is at most 1.1 scaled ULP from the
truth, against CmdStan's 0.9 to 156. On the ch12 models the merged order is
the worse one on 54 of 75 distinct values that differ, for a reason not found;
on `ldaK2` and `dogs` it is the better one overall.

One of the 25 (`GLMM1_model` lp) has a sharper cause. Stan Math's vectorized
`poisson_log_lpmf` computes `sum(n*alpha) - sum(exp(alpha))`, two sums that
cancel from about 2,900 and 2,300 to 625. Two entries (`normal_mixture_k`
grad[4], `epinow2_estimate_secondary` grad[3]) do not move under any switch.

## Proposed fixes, not made

- Several accumulators or a pairwise sum where a broadcast scalar's adjoint is
  reduced (`mul_bwd` and its siblings in `eltwise_expr.cpp`, the scalar branch
  of `density_bwd`).
- Neumaier compensation in the scatter loops (`ordered_logistic` cut points in
  `density_fused.cpp`, the `GATHER` adjoint).
- A compensated `SUM_VEC` for the target terms that re-roll and partition
  build, leaving a user's `sum()` alone.
- One summed pass in a stanli `poisson_log` (and `poisson`) kernel.

These move values towards the truth and away from CmdStan's bits, so they do
not shrink the ledger, and a fix applied to every call (not only to the
reductions that re-roll and partition create) could add entries. 32 of the 89
comparable `main` models with a value over 10 raw ULP from CmdStan lose every
such value when merging is off. The difference from CmdStan on those models is
the merged summation order.

## Tools

`tools/hp/ledger_accuracy.py` scores the ledger, with `--switch` and
`--census`; `tools/hp/hp_eval.py --grad-index` adds coordinate derivatives
for models above the full-gradient size, which raised the scored entries
from 81 to 117.
