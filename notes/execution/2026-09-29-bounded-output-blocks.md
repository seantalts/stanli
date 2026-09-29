# Bounded generated-quantity blocks

Bounded real temporaries can now stay in the existing structured engine, so
a changing local array length no longer sends an otherwise supported output
block through MIR. The ordinary graph continues around the closed block.

Continuation of the [inventory and probe](2026-09-29-execution-inventory-and-bounded-storage.md).
The user accepted the closed-block direction after the Fable comparison.
Baseline: `aca1a408`, including PR #411, synchronized with upstream
`dacfaae1`. Native Release baseline library SHA-256:
`6eb58e553e22d577e358111a731c691efbd134470cd25cac15f63f720a2e3abf`.

## Admission contract

Use the existing structured engine for a lexical generated-quantity block
that owns bounded one-dimensional real arrays and returns scalar values.
Initially accept scalar arithmetic, comparisons, conditionals, bounded `for`
loops, single-element reads/writes, `sum`, `size`, and `num_elements`.
Reject array copies, slices, other container types, calls with effects,
solvers, user function calls, early exits, and unbounded loops. Inlining may
already have removed a user function call; recognition depends on MIR semantics.

Prove nonnegative extent ranges before omitting size validation. Preserve
declaration snapshots, initialization sentinels, and logical bounds checks.
Only scalar external results cross the boundary; no dynamic shape escapes.
Candidate construction uses the existing private lowering fork and publishes
nothing until all admission checks pass. Import only referenced parent values.

Provisional admission limits: 256 graph slots, 8,192 reserved double elements,
4,096 worst-case statement visits, and 65,536 loop-weighted slot elements.
These bound preparation/storage/work proxies, not total process RSS. Slot
checks precede allocation of fills. Count both branches conservatively and
multiply nested loop bounds. Keep existing paths for blocks without a
runtime-dependent local extent. Refusal retains the established fallback.

## Evaluator and decision gates

- Ordinary optimized source, no artificial loop wrapper or forced selector.
  Reuse the existing fallback fixture and add a structurally different case.
- Compare repeated changing inputs and seeded output rows with forced MIR;
  verify zero/one/maximum sizes, snapshot mutation, partial initialization,
  constant/dynamic bounds errors, and RNG before/after a failing region.
- Refuse negative or excessive capacities, excessive loop work, array copies,
  unsupported consumers/types, and effects. Check surrounding state on refusal.
- Independent CmdStan values and the recorded corpus remain numerical gates.
- Six alternating fresh-process pairs measure source/preparation, first and
  warm outputs, gradients, short inference, and peak memory. Include ordinary
  graph/register/structured canaries, changing-input rows, and capacity stress.
  Retain only measured gains without a clear normal-use regression. Finite
  canaries cannot establish a universal performance guarantee.

The fixed-branch expansion is the already-measured alternative: similar warm
cost, less startup work, but duplicated code for each possible shape. A local
MIR call remains deferred because it adds a new boundary without removing this
case's interpreted arithmetic. Neither alternative is implemented here.

## Representation experiment

The first candidate kept the existing execution-path cache. Six alternating
fresh-process pairs with longer inference timing windows showed 1.125→0.914 ms
and 3.610→3.393 ms for 1/32 following binomial outputs. But rebuilding the path
when a two-element local changed length on every call nearly consumed the
benefit: 6.46→6.28 µs per complete row.

The existing `STANLI_NO_STRUCTURED_REPLAY` ablation identified the cost. Running
this same structured tree without recording a reusable execution path reduced
those inference times to 0.571/2.970 ms and alternating-length rows to 0.97 µs.
This is an existing execution path, not a new evaluator. The production
candidate now chooses it only for these newly admitted small output blocks;
other loop plans keep their original policy. No new environment switch is added.

## Final validation and native results

A clean native rebuild passes all 316 CTests, including the new row/error/RNG
checks and refusal-prefix comparisons. The independent CmdStan fixture checks
18 density/gradient/output values at three points, with maximum error 1 ULP.
All 329 recorded corpus models pass existing gates (1,020,194 values); the
previous 7,040-ULP cancellation exception is unchanged. No tolerance widened.
The 373-case static selection replay changes only `gq_partial_fallback`, from
whole-output MIR to a compiled graph containing one structured block. This
is a static census, not a frequency measurement.

Final library: `f2c732d1261d4bde6958bac403e6f24d0e7ee6079856a45158207e725ebc3a04`,
39,834,576 bytes, **3,248 bytes larger** than the PR #411 baseline.
Both builds use native Darwin arm64, AppleClang 21, Release `-O3 -DNDEBUG`,
threads enabled, and full LP. No browser performance claim is made.

Six alternating fresh-process pairs/triples use ordinary optimized MIR and
default runtime settings. Times below are medians in microseconds, shown as
MIR baseline → bounded block. Source compilation is measured separately in
the evidence; the compiler itself is unchanged.

| Following binomial outputs | Preparation | First output row | Warm output row |
| --- | ---: | ---: | ---: |
| 1 | 271.57 → 274.40 | 25.44 → 38.10 | 5.99 → 0.78 |
| 32 | 268.21 → 278.44 | 20.27 → 35.71 | 8.25 → 2.47 |
| 1024 | 390.68 → 371.76 | 94.48 → 87.33 | 74.61 → 54.46 |

For 1/32 outputs, the additional preparation-plus-first-row cost is recovered
in approximately 3/5 subsequent steady-shape rows. These are measured
break-even estimates, not a promise for every input. Peak process RSS in the
phase runs increases by about 0.18–0.31 MiB (process totals about 27 MiB).

Short single inference measurements were noisy, so all four canaries and
both target sizes were repeated in 1-second timing windows, with 250-ms
warmup, in six alternating fresh-process pairs. Each inference includes
100 warmup iterations, 100 draws, and output generation. These are warmed
inference timings, excluding source compilation/preparation/first-use costs.

| Model | Baseline median ± MAD (ms) | Candidate median ± MAD (ms) |
| --- | ---: | ---: |
| block1 | 1.132 ± 0.003 | 0.572 ± 0.003 |
| block32 | 3.577 ± 0.040 | 2.968 ± 0.012 |
| ar1 | 7.879 ± 0.012 | 7.894 ± 0.040 |
| ode_nested_for | 68.614 ± 0.396 | 68.475 ± 0.341 |
| gq_container_rng_complete | 1.051 ± 0.006 | 1.063 ± 0.005 |
| structured_local_shape | 175.409 ± 2.203 | 173.799 ± 2.296 |

The ~1% RNG-canary difference did not repeat in the identical-binary control
run: the two baseline groups had medians 1.070/1.063 ms, versus 1.065/1.061 ms
for the two candidate groups. Across the repeated runs there is no consistent
normal-use regression. This finite sample is not a universal guarantee.

The separate bounded-array fixture changes the parameter value on every call
and its sign/array length on every call or every 16 calls. Median complete
row times (µs), again baseline → candidate:

| Maximum array length | Length changes every row | Every 16 rows | Admission |
| --- | ---: | ---: | --- |
| 2 | 11.35 → 0.93 | 10.97 → 0.91 | compiled |
| 32 | 32.64 → 1.75 | 32.56 → 1.51 | compiled |
| 128 | 100.55 → 3.68 | 104.18 → 3.39 | compiled |
| 512 | 386.53 → 376.82 | 392.45 → 387.90 | refused; both use MIR |

The 512-capacity case exceeds the loop-weighted storage budget; its small
timing difference is not a compiled-path improvement. Repeated changing-input
process RSS stays around 16–17 MiB, with candidate medians about 0.1–0.3 MiB
higher. Those runs omit source compilation and must not be compared directly
with the phase-run RSS totals. The limits bound admission proxies; they do
not specify a total RSS ceiling.

[Raw evidence](data/2026-09-29-bounded-output-blocks.json.gz) retains every
sample, initial recording-path results and ablation, binary/source hashes,
fixtures, static selections, validation output and protocols. Exploratory
runners remain outside production/test source under `.cache/`. The checked-in
regressions and benchmark entry exercise the default enabled implementation.

## Remaining scope

This removes one concrete whole-output fallback without adding an execution
engine. It does not extend transformed parameters, active callbacks, dynamic
external results, other container/element types, arbitrary consumers, effects
inside the region, or unbounded storage. Expand those only with their own
logical-shape/effect/derivative proofs and a measured workload. The conservative
limits and grammar intentionally retain the existing fallback on refusal.
