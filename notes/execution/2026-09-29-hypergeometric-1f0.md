# Shared scalar support for hypergeometric_1F0

Base: `bb3da1db446ee26272077297def091a00cf9e8a3`, after merging #416.
This closes one missing function in checklist section 3.1. It does not remove
an existing MIR fallback: the previous runtime could not evaluate the function
in MIR either. Native performance and coverage, rather than interpreter counts,
remain the reason for this work.

## Refreshed gap probes

The existing conformance classifications were checked against current code
with one valid expression per function in five contexts: ordinary model code,
parameter-dependent runtime control, generated quantities, transformed data,
and the standalone function API. All 50 baseline probes failed. The function
list was `hypergeometric_1F0`, `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`,
five-argument `wiener_lcdf_unnorm` and `wiener_lccdf_unnorm`, four-argument
`gp_periodic_cov`, and the three `discrete_range` CDF variants.

These are refreshed examples, not a complete overload or solver-context census.
The Wiener functions have additional overloads, the covariance generator has
separate shape gaps, and the all-integer CDFs are classified as inapplicable by
the gradient harness. None of those classifications establishes support.
Generated-quantity failure can appear in a `WANAMES FAIL` message even with a
zero process exit status; the probe's output must be inspected as well.

After this change all five `hypergeometric_1F0` probes succeed; the other 45
remain failures. Raw inputs, outputs, scripts and library identities are in
the [evidence archive](data/2026-09-29-hypergeometric-1f0.json.gz).

## Implementation and execution

The pinned Stan signature is `hypergeometric_1F0(real, real) => real`. A new
opcode and one entry in the existing scalar-binary registry supply its shared
kernel, graph lowering, register CALLs, structured-loop calls, MIR evaluation
and standalone calls. There is no separate engine, opt-in implementation or
new numerical formula. The internal kernel's broadcast tests do not imply
new container overloads in the Stan language.

Both the forward computation and derivatives use pinned Stan Math. In
particular, Stan requires `abs(z) < 1`; replacing the function with an
algebraically related expression would not preserve its checks. Mixed active
and data arguments continue to select Stan's corresponding overloads. The
existing local autodiff tape computes the kernel's reverse pass. Binary-kernel
registration now labels that mechanism `nested_tape` in execution diagnostics.

The initial selection assertion expected a retained loop for a small bounded
model. It failed because the existing bounded-specialization optimization
expands this model into graph/register regions. No admission rule
was changed. Tests now cover the default selection and the retained path with
that optimization disabled. A separate parameter-dependent while-loop probe
selects the structured engine automatically. Preparation and constrained
initialization still use their existing MIR machinery.

## Correctness evidence

- Clean rebuild after the opcode-table change; 327/327 CTests pass.
- Focused CmdStan references compare log density, both gradients, output names
  and every output value at three points, including both parameter-selected
  branches. Automatic, register and retained-loop configurations all pass the
  existing 10-ULP gate. No tolerance was widened.
- The shared kernel tests cover both active inputs, either input active,
  non-unit/non-finite adjoint seeds, aliases, internal broadcasting, boundaries
  just inside and at `z = +/-1`, outside-domain values, NaNs and tape recovery.
  MIR double/var evaluation and compiled standalone rejection/recovery also pass.
- Execution tests assert graph, register and structured selections, compiled
  outputs, and no MIR entry during repeated gradients or standalone calls.
- The maintained signature generator adds the scalar signature and its two
  single-active cases: 12,565 builtin cases. All 73 builtin/density reference
  models pass their existing gate. Adding three sorted cases shifts 23 builtin
  partitions; their references were independently recompiled with the same
  pinned CmdStan toolchain. Historical conformance classifications are preserved.
- All 329 recorded corpus models pass, comparing 1,020,194 values. Existing
  scaled-error/ULP rules and the previously recorded 7,040-ULP cancellation case
  remain unchanged; this is not a universal 10-ULP claim.

## Native measurement

Measurements use Release AppleClang 21 on Darwin arm64, full log densities and
thread-enabled runtime, with six alternating fresh-process baseline/candidate
pairs. Builds and tests finish before timing. The baseline library contains
the final #416 implementation; its exact identity and earlier source diff are
preserved in the [refusal-cost record](2026-09-29-bounded-refusal-cost.md).

The phase tool measures warmed source compilation/preparation, first and warm
gradients/output rows, and whole-process peak RSS. A separate fresh-process run
measures preparation, seeded 100-warmup/100-draw inference, output rows and
destruction, excluding source compilation, file reads and library loading.
The new function fixture is a synthetic evaluator test, so only its evaluation
phases are timed; there is no successful pre-change baseline for that function.

Primary results, median ± MAD; preparation in microseconds and complete
inference in milliseconds:

| Workload | Preparation before → after | Complete inference before → after |
| --- | --- | --- |
| AR(1) | 148.576 ± 3.741 → 151.738 ± 4.099 | 8.960 ± .122 → 8.793 ± .058 |
| Eight schools, noncentered | 74.884 ± 1.059 → 75.780 ± 2.953 | 1.933 ± .026 → 1.918 ± .040 |
| Nested real output | 334.771 ± 13.265 → 331.163 ± 5.361 | 1.177 ± .013 → 1.190 ± .014 |
| Integer output | 259.703 ± 6.823 → 259.431 ± 9.308 | 1.050 ± .007 → 1.055 ± .005 |
| Oversized nested refusal | 304.555 ± 6.978 → 297.400 ± 9.878 | 86.893 ± 3.975 → 82.705 ± 2.142 |
| Nested-loop ODE | 427.261 ± 5.427 → 402.045 ± 8.452 | 65.315 ± .325 → 72.357 ± .407 |

The apparent ODE inference regression required investigation. A bounded repeat
with sampled-draw hashes gave 69.119 ± 3.740 → 69.277 ± 3.422 ms; both libraries
produced identical draws and final output rows. An A/A control using the same
baseline library for both labels gave 70.320 ± 3.136 → 67.713 ± 2.893 ms. Times
in both groups cluster around 65 and 73 ms. The eight-schools repeat also moved
in the opposite direction from its primary comparison, while draws stayed
identical. These short fresh-process timings do not isolate a stable effect
from the code change. No timing sample was excluded based on its direction.

The initial Python-boundary measurements also showed small differences in the
eight-schools gradient and ODE output row. A native C-API loop removes Python
call overhead and uses 200 ms warmup plus 500 ms measurement:

| Native phase, microseconds | Before | After |
| --- | ---: | ---: |
| Eight-schools gradient | .22974 ± .00139 | .23157 ± .00102 |
| ODE gradient | 85.575 ± .703 | 85.779 ± .588 |
| ODE output | 36.501 ± .321 | 36.593 ± .201 |

For comparison, native A/A medians were .23176 → .23319 microseconds for the
eight-schools gradient and 36.780 → 36.680 for the ODE output. The observed
candidate differences are below 1% and comparable to the same-binary controls.
No stable ordinary-use slowdown was isolated, and no canary speedup is claimed.
This evidence cannot exclude smaller effects or guarantee every model's timing.

The new 40-iteration function fixture takes 652.549 ± 21.387 microseconds to
prepare, 28.959 ± .709 for its first gradient, 6.054 ± .105 per warm gradient,
3.917 ± .438 for its first output row, and .492 ± .010 per warm row. Warm source
compilation is 1.575 ± .053 ms. These are absolute costs of newly supported
work, not before/after gains.

The library increases from 39,856,384 to 39,875,552 bytes: +19,168 bytes,
about 0.05%. Peak RSS is a whole-process observation, not a measurement of
retained executor storage. The evidence is limited to the measured workloads.

The first timing attempt stopped on an incorrect eight-schools source path
before measuring that model. Its completed AR(1) pairs were retained; the
corrected run used the recorded noncentered corpus model and continued with
the remaining cases. The failed run is preserved in the evidence archive.

## Remaining work

The other numerical functions need shared-kernel implementations and independent
oracle cases. `inc_beta` and `inv_inc_beta` are candidates for the existing
three-input builtin contract; `hypergeometric_2F1` fits the four-input contract.
Preserve mixed-activity behavior and measure binary costs before adopting them.
Extended Wiener and DLM calls exceed today's six-input operation limit and
need a separately reviewed packing contract. Complex/tuple values, dynamic
storage and recursive calls remain distinct design decisions in the checklist.
