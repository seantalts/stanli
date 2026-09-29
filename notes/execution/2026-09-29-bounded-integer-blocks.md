# Bounded integer output blocks

One-dimensional integer arrays can now use the existing bounded-output engine.
A loop must fill every live element before a sum, and the compiler must prove
that every possible partial sum fits in int32. This extends the
[real-container path](2026-09-29-bounded-vector-blocks.md) without adding an
engine, runtime switch, or experimental production code.

Baseline: PR #413, merged as `b2fe8596c11fd2ff7c7069195a4b8f156ecf0593`.
Its tested native library is SHA-256
`f522549e0b108e600ba8590bfc4718bfd5e4ab0f73f105613a9688a37c21e5b9`.

## Proof and limits

The admitted construction is `for (i in 1:size(a)) a[i] = expression`, with
one unconditional write to `a`. The expression may read the iterator and
values that the body neither declares nor changes. Extra writes, conditional
fills, self-reads, and loop-carried inputs do not establish a fill proof.
A later loop that mutates the array invalidates its earlier value range before
lowering the body. This prevents a sum inside that loop from using bounds
that described only the first iteration.

Initialization covers the current logical length, not the unused capacity.
The initialization metadata distinguishes those facts; existing physical-prefix
consumers cannot mistake the logical proof for initialized padding. Branch
joins require ranges from both arms. Integer sums use the existing guarded
sum lowering and `OP_SUM_VEC`, including when the result is promoted to real.
A conservative bound on all negative and positive contributions proves int32
partial-sum safety; double storage then represents those integers exactly.
The result range includes zero when the logical length may be empty.

The existing closed grammar and resource budgets remain unchanged. This slice
does not handle all legal fill patterns: reversed or partial fills, extra
writes, repeated local names, nested containers, copies, slices, or effects
inside these blocks still need separate proofs. Potential integer overflow
keeps the fallback; upstream C++ overflow has no portable numeric oracle, so
those refusal cases are compiled but never evaluated in the new tests.

The alternative of generating a separate static block for each length was
already evaluated in the preceding local-storage work. This slice needs no
new dynamic-value representation.

## Validation

- All 319 native CTests pass. Focused tests cover zero/one/multiple live
  elements, changing input signs, extent snapshots, both-arm fills, promoted
  sums, sums used as subsequent extents, errors and seeded RNG continuation.
- Ten adversarial construction/overflow cases refuse, and their complete
  compiled prefixes match the engine-disabled baseline.
- The independently recorded CmdStan fixture checks 18 values with maximum
  error 1 ULP; CmdStan also independently rejects an out-of-range read.
  Existing tighter and exception-specific gates are unchanged.
- All 329 recorded corpus models pass, covering 1,020,194 values. The existing
  7,040-ULP cancellation exception is unchanged; no tolerance was widened.
- All 373 prior static execution selections match #413. These are selection
  checks, not a measurement of runtime fallback frequency.

## Native measurements

Six alternating fresh-process pairs use native Darwin arm64 Release,
AppleClang 21, threads enabled, full LP, and Python/ctypes entry points.
Phase windows are 200 ms warmup / 250 ms measurement. Source compilation
is unchanged; those samples are retained with the other raw measurements.
The same model is measured with an empty or two-element steady input;
its other branch has eight elements. Times below are median microseconds.

| Steady input | Preparation | First gradient | First row | Warm row |
| --- | ---: | ---: | ---: | ---: |
| Empty | 247.66 → 276.51 | 5.63 → 11.21 | 38.56 → 34.17 | 8.14 → 0.64 |
| Two elements | 253.17 → 264.66 | 5.50 → 13.12 | 38.19 → 32.15 | 9.91 → 0.77 |

The nonempty warm row improves about 12.9×. Including preparation, first
gradient, and first row, the added startup cost is recovered after about two
subsequent nonempty rows (about four for the empty case). Phase process peak
RSS increases about 0.13–0.16 MiB to roughly 27 MiB. This is whole-process peak
memory, not a measurement of retained executor storage. The storage/work
budgets are unchanged.

Complete inference uses separate fresh processes, 250 ms warmup and one-second
windows. Each invocation resets the seed and runs 100 warmup iterations,
100 draws, and their output generation. These warmed timings exclude source,
preparation and first-use costs. The sampler exercises both lengths. Values
are median ± MAD in milliseconds.

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| gq_bounded_integer | 1.540 ± 0.006 | 0.311 ± 0.001 |
| integer_nonempty | 1.621 ± 0.012 | 0.316 ± 0.001 |
| ar1 | 7.877 ± 0.022 | 7.954 ± 0.023 |
| ode_nested_for | 67.915 ± 0.105 | 67.396 ± 0.582 |
| gq_container_rng_complete | 1.054 ± 0.004 | 1.059 ± 0.003 |
| gq_bounded_block | 0.298 ± 0.002 | 0.296 ± 0.003 |
| execution_rng | 0.786 ± 0.004 | 0.790 ± 0.003 |
| gq_bounded_vector | 0.358 ± 0.003 | 0.354 ± 0.001 |
| gq_bounded_integer_refusals | 1.070 ± 0.008 | 1.068 ± 0.003 |

Target inference improves 4.9–5.1×. Initial canary signals were RNG rows
9.86 → 10.28 µs, existing vector rows 0.870 → 0.902 µs, and ar1 inference
7.877 → 7.954 ms. Six additional counterbalanced baseline A/B and candidate
A/B groups used one-second windows. All samples remain in the evidence.

| Confirmation (µs) | Baseline A / B | Candidate A / B |
| --- | ---: | ---: |
| gq_container_rng_complete | 10.134 / 10.120 | 10.107 / 10.047 |
| gq_bounded_vector | 0.875 / 0.872 | 0.881 / 0.882 |
| ar1 | 7883.428 / 7906.183 | 7885.528 / 7920.814 |

The initial differences did not repeat consistently. The residual vector-row
difference is about 0.008 µs, comparable with its 0.010–0.011 µs baseline
MAD; its primary inference timing improved. There is no resolved normal-use
regression in these measurements. Finite canaries cannot prove that no model
will ever regress.

Another six-pair run changes the parameter and length on every row,
alternating zero and the listed maximum. Median complete-row times in µs:

| Maximum length | Baseline → candidate | Selection |
| --- | ---: | --- |
| 1 | 8.53 → 0.84 | Existing structured engine |
| 2 | 9.49 → 0.90 | Existing structured engine |
| 32 | 22.95 → 1.41 | Existing structured engine |
| 128 | 66.47 → 3.27 | Existing structured engine |
| 512 | 261.66 → 262.43 | MIR in both |

Capacity 512 exceeds the existing work budget; its timing difference is not
a compiled-path gain. The candidate native library is 39,852,688 bytes,
18,288 bytes (0.046%) larger than baseline, SHA-256
`5193b815ddfd354abeed9ee299cbda0831d4879ddb68e53aaa2e932009b3e7af`.
No dependency is added. Compressed package size was not measured here.

[Raw evidence](data/2026-09-29-bounded-integer-blocks.json.gz) retains 252
primary samples, 24 nonempty follow-ups, 72 controls, input and source hashes,
binary identities, build settings, runner protocols, selection reports,
validation logs and the independent reference. The maintained benchmark
manifest adds the admitted integer model and a refusal canary; public corpus
benchmark tables are unchanged. Exploratory runners remain under `.cache/`.
