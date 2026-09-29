# Teaching-model performance investigation: retained conclusions

Historical investigation for PR #371 and follow-ups #372–374. The initial
baseline was `a77e29ab` (runtime/compiler equivalent to `2ae6c1d0`); the final
published checkpoint in this record is run `fd5e0ecacddc7047`, runtime
`6e462c2e`. These are historical identities, not the current branch.

## Evaluator

The experiment measured whole CLI inference: preparation, warmup, 1,000 draws,
generated quantities, and CSV output across four seeds. Its cap was
min(3× CmdStan, 900 seconds). Missing/capped cases remained in the inventory;
fixed-point gradients and profiles attributed costs but did not substitute
for inference. The external numerical gate was 1e-9 scaled error with existing
exceptions; tighter internal parity, effects and refusal tests remained.
Small apparent regressions required bounded counterbalanced A/B and A/A runs.

The later user target was practical end-to-end parity rather than winning every
microbenchmark. That historical target does not override current priorities or
turn measurement noise into an acceptable regression budget.

## Results and reusable lessons

- Preparation recursion, static GP geometry, scalar indexing and density
  kernel work were separate hypotheses. Select changes from measured phase
  costs, not from a model name or the number of calls.
- Bounded whole-model specialization had to roll back completely when range
  or gather selectors exposed arithmetic-order differences. An observed
  one-ULP cross-path change led to narrower admission, not wider tolerance.
- Shared-vector probit reached 15.87 → 4.55 µs versus CmdStan 5.24 µs in the
  recorded five-pair local comparison, with bitwise fixed-point parity.
- Wiener partial reuse preserved weighted arithmetic only for a unit output
  seed. Even finite extreme seeds could change Inf/NaN behavior when a unit
  partial was post-scaled. Other weights and nonfinite partials retained
  replay. The corrected repeat measured 45.02 → 29.23 µs versus CmdStan
  28.99 µs: near parity, not a stable claim of beating CmdStan.
- A native-conditional COM-Poisson attempt did not solve its dynamic integer
  requirement and was removed. Later COM-Poisson support and measurements
  are in the [brms report](../../../brms-performance.md).
- Sequence flattening preserved tested values but missed its predeclared 5%
  benefit criterion on GEV/ALD cases and a canary; the prototype was removed.

## Final complete checkpoint and limits

Run `fd5e0ecacddc7047` covered 199 fixtures: 193 complete comparisons, 187
lower Stanli medians, six slower, one capped, and five failed. Four completed
cases remained below a 0.8 CmdStan/Stanli ratio: GEV 0.420, asymmetric Laplace
0.677, zero-inflated asymmetric Laplace 0.700, and mixture theta 0.771.
Severe sampler diagnostic flags in the mixture and capped negative-binomial
case require separating trajectory work from engine throughput. All 62
Rethinking fixtures exceeded 1.0 in that sweep. Subsequent GP checks were
kept separate; they did not rewrite the frozen aggregate.

The published [historical teaching report](../../../../output/teaching-performance/README.md)
and [brms results](../../../brms-performance.md) retain the measured outcomes.
The 550,455,487-byte local raw archive had SHA256
`30840d8b8ed7ae4734ec3d69f41fff54bad51c91d9f4cf2227e00667182125ad`;
its original local paths and all intermediate identities are in the full record.
Availability of ignored local evidence is not guaranteed in a fresh checkout.

## Validation lessons

A completed specialized graph emits a different profile stage from the fallback;
consumers must select the completed graph, never mix partial/refused stages.
The old 30-second compiler timeout caused measurement failures and was restored
to the 300-second reference budget without changing numerical gates. Windows
R acceptance required the CLI beside its bundled compiler and DLLs; skipping
the independent CSV oracle would not establish compatibility.

The recorded full-platform run at `f5b047de` passed Linux/macOS/Windows builds,
compiler parity, ASan, TSan and R acceptance. Later local checks and partial
runs are separate evidence. Open support/performance gaps prevented a blanket
full-parity claim. This digest does not certify the current build.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/docs/superpowers/plans/2026-09-15-teaching-performance.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
