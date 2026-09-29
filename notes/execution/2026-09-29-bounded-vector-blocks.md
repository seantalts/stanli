# Bounded vector output blocks

The bounded generated-quantity path now accepts owned real vectors and row
vectors as well as real arrays. It uses the same structured engine, execution
policy, and resource limits introduced in [the previous slice](2026-09-29-bounded-output-blocks.md).
No new engine, runtime switch, or disabled experimental code is added.

Baseline: merged PR #412 at `50c1b14b6a46e80929db9e0cd01431ec119c92ae`.
The baseline library is the identical tested #412 build, SHA-256
`f2c732d1261d4bde6958bac403e6f24d0e7ee6079856a45158207e725ebc3a04`.
Both #411 and #412 were merged before completing this follow-up.

## Proof and scope

The same closed grammar accepts scalar arithmetic/control, bounded loops,
single-element reads/writes, sums and shape queries. Vectors keep one runtime
length regardless of orientation: rows(vector) and cols(row_vector) read that
length; their opposite dimension is one. Shape-query range analysis uses the
same rule as execution. Declaration lengths remain snapshots.

Named vector storage carries an owning Eigen layout in these bounded output
blocks. Its sum therefore keeps Stan's packet grouping even after indexed
writes. This metadata change is scoped to these blocks; existing retained-loop
plans keep their previous policy. A sum with capacity one stays scalar even
when the input is empty. The correction names the sum opcode, preserving
existing orientation-free intermediates for other operations.

A differential test also found that MIR silently grew a vector on an
out-of-bounds single-element write. CmdStan rejected that write, as the compiled
path already did. MIR now validates the logical first extent before writing,
including matrix-row writes, and never grows storage through this assignment.
This is a correctness fix to the fallback, not permission to match its former
behavior. The raw evidence includes CmdStan's independent rejection.

Copies, slices, integer/nested containers, other consumers, effects inside the
block, active callbacks, and external dynamic results remain separate work.
Their established fallback remains. The shape-expanded alternative would
duplicate the code for each possible length; the preceding experiment already
measured that alternative. This extension reuses the existing bounded-storage
contract instead of introducing another representation.

## Validation

- All 318 native CTests pass, including repeated changing lengths, zero/one
  capacity, declaration snapshots, bounds failures, partial initialization,
  seeded whole-row/RNG continuation, and refused-trial graph comparisons.
- Two independent CmdStan fixtures check 36 values, including cancellation in
  Eigen sums, with maximum error 1 ULP. Out-of-bounds vector/row-vector writes
  reject independently in CmdStan.
- All 329 recorded corpus models pass the existing gates, covering 1,020,194
  values. The existing 7,040-ULP cancellation exception remains unchanged;
  no tolerance was widened.
- All 373 prior static execution selections match #412. New fixtures prove
  default admission separately; a static census is not a runtime frequency
  measurement.

## Native measurements

Six alternating fresh-process pairs used native Darwin arm64 Release,
AppleClang 21, threads enabled, and full LP. Source, preparation, first and warm
evaluations are separate. Phase warmups/timing windows are 200/250 ms. Source
compilation is unchanged and its measurements are retained in the raw data.
Times below are medians in microseconds, baseline → candidate.

| Fixture | Preparation | First gradient | First output row | Warm output row |
| --- | ---: | ---: | ---: | ---: |
| gq_bounded_vector | 249.48 → 275.31 | 5.60 → 11.90 | 33.17 → 38.65 | 13.91 → 0.87 |
| gq_bounded_row_vector | 244.36 → 266.27 | 5.85 → 12.54 | 38.12 → 42.04 | 13.77 → 0.90 |

The added preparation, first-gradient, and first-row cost is recovered after
about three subsequent rows in these steady-input measurements. Peak phase
process RSS increases by about 0.31–0.33 MiB, to roughly 27 MiB. These are
whole-process peaks, not exact retained executor memory measurements. No
storage/work budget is increased.

Complete inference timings use separate fresh-process pairs, with 250 ms of
warmup and one-second timing windows. Each call resets the seed and performs
100 warmup iterations, 100 draws, and output generation for those draws.
They exclude source/preparation/first-use costs. Values are median ± MAD in ms.

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| gq_bounded_vector | 2.757 ± 0.012 | 0.358 ± 0.001 |
| gq_bounded_row_vector | 2.767 ± 0.004 | 0.356 ± 0.003 |
| ar1 | 7.877 ± 0.031 | 7.921 ± 0.034 |
| ode_nested_for | 68.800 ± 0.192 | 67.861 ± 0.172 |
| gq_container_rng_complete | 1.066 ± 0.005 | 1.056 ± 0.004 |
| gq_bounded_block | 0.296 ± 0.001 | 0.299 ± 0.001 |
| execution_rng | 0.782 ± 0.004 | 0.792 ± 0.010 |
| gq_bounded_vector_refusals | 1.033 ± 0.004 | 1.032 ± 0.005 |

The initial RNG-canary warm-row result was 9.62 → 10.22 µs, while its complete
inference timing improved slightly. Small positive inference differences also
appeared for the old bounded-array and MIR RNG canaries. The predeclared
confirmation used six counterbalanced groups of baseline A/B and candidate A/B,
with longer one-second windows. No samples were removed. Group medians:

| Control | Baseline A / B | Candidate A / B |
| --- | ---: | ---: |
| RNG row (µs) | 10.18 / 10.09 | 10.13 / 10.09 |
| RNG inference (µs) | 1063.85 / 1055.66 | 1062.15 / 1061.77 |
| Bounded-array inference (µs) | 296.95 / 297.69 | 296.62 / 295.98 |
| MIR RNG inference (µs) | 779.61 / 789.70 | 787.00 / 783.16 |

Those differences did not repeat consistently. The still-interpreted fixture
with indexed writes also shows no clear slowdown. The measured canaries do not
show a reproducible normal-use regression; they cannot establish a universal
no-regression guarantee. Keep all measurements, including the initial signals.

A separate test changes the input value and length on every row, alternating
between zero and the listed maximum. Median complete-row times in µs:

| Maximum length | Vector baseline → candidate | Row vector baseline → candidate |
| --- | ---: | ---: |
| 1 | 10.16 → 0.88 | 10.36 → 0.89 |
| 2 | 11.31 → 0.92 | 11.38 → 0.90 |
| 32 | 40.42 → 1.45 | 40.51 → 1.50 |
| 128 | 136.22 → 3.33 | 132.45 → 3.39 |
| 512 | 523.38 → 534.57 | 528.56 → 510.96 |

Both 512-capacity cases refuse the existing work budget and still use MIR;
their timing differences are not compiled-path gains. Sizes 1–128 use the
existing structured engine. The final library is 39,834,400 bytes, **176 bytes
smaller** than the baseline, SHA-256
`f522549e0b108e600ba8590bfc4718bfd5e4ab0f73f105613a9688a37c21e5b9`.

[Raw evidence](data/2026-09-29-bounded-vector-blocks.json.gz) contains all 312
primary samples and 96 controls, input text/hashes, binary/source identities,
runner protocols, selection reports, validation logs and independent reference
records. Only enabled production code, exercised fixtures, and maintained
benchmark entries ship; exploratory runners remain under `.cache/`.
