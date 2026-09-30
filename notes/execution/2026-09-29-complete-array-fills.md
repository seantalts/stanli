# Complete array-fill proofs

Bounded integer output arrays can now be filled through both arms of an
`if/else`, and a full fill can coexist with additional safe writes. This uses
the existing structured engine and integer sum kernel. No execution engine,
runtime option, work budget, or container representation changes.

Baseline: PR #414, merged as `0a01f56545e995a131af1d32e803a4df5dbfe42a`.
The matched native baseline library is SHA-256
`5193b815ddfd354abeed9ee299cbda0831d4879ddb68e53aaa2e932009b3e7af`.
See the [preceding integer-array result](2026-09-29-bounded-integer-blocks.md)
for logical initialization and int32 sum contracts.

## Proof and scope

For `for (i in 1:size(a))`, or `num_elements(a)` on a one-dimensional array,
the compiler checks that every iteration writes `a[i]` on every path. A
sequence guarantees a write if one statement guarantees it. An `if/else`
guarantees a write only when both arms do. A nested loop does not establish a
guaranteed write, because it might execute zero times.

Every write to the array contributes to the value-range union, including
conditional writes and writes inside nested loops. An extra write cannot
silently narrow the proven range. All written expressions must depend only on
the current iterator and values the body does not declare or change. The
existing sum guard then proves safety of all possible int32 partial sums.
The proof runs after lowering, while the iterator bounds are still available;
it does not execute or rewrite the body, omit reads, or move evaluations.
Runtime branches may still inspect earlier array contents. Initial sentinel
values, exceptions, evaluation order, and RNG continuation remain observable.

The same proof covers a fixed one-element array inside a block containing
other changing-size storage. The former bounded single-write proof is replaced;
non-bounded retained-loop policy is unchanged. The earlier refusal fixture's
safe-overwrite mode now compiles by default.

Missing branch writes, reversed fills without a direct complete fill, mutable
or branch-assigned local inputs, array self-reads on the right-hand side, and
possible sum overflow still refuse. Safe sequential writes after a completed
loop still need a separate range-update proof. Nested containers and effects
inside bounded blocks remain separate work.

Turning each branch into a ternary assignment would add a program rewrite
without improving execution: the existing engine already executes both forms.
The compile-time proof is sufficient for this slice. General dynamic values,
new dispatch machinery, and per-length code duplication remain unnecessary.

## Validation

- All 321 native CTests pass. Final focused checks also pass after adding
  array-dependent branch predicates, nested overwrites, and mixed fixed/dynamic
  arrays. Nine admitted modes cover changing lengths 0/1/2/8/32 and bounds errors,
  with exact repeated full-row and RNG-continuation parity against MIR.
  Branch/overwrite tests also cover capacity 128 and transactional budget
  refusal at 512.
- Ten new adversarial cases refuse transactionally; complete compiled prefixes
  match the disabled path. Positive and negative overflow cases are checked
  for admission only; upstream C++ integer overflow has no portable oracle.
- Two independent CmdStan references cover the branch-fill model and the
  previously refused safe-overwrite fixture: 36 values, maximum 1 ULP.
  CmdStan independently rejects the out-of-range read too. No tolerance is widened.
- All 329 recorded corpus models pass: 1,020,194 values, with the existing
  7,040-ULP cancellation exception unchanged. All 373 prior surveyed selections
  match #414; those historical inputs predate the new targeted fixtures.
- A simple local-variable negative probe was optimized away by stanc and
  legitimately admitted. The durable refusal instead retains a branch-assigned
  local value in MIR, so the test exercises the actual missing range proof.

## Native measurements

Six alternating fresh-process pairs use native Darwin arm64 Release,
AppleClang 21, threads enabled, full LP, and Python/ctypes entry points.
Phase windows are 200 ms warmup / 250 ms measurement. Source compilation
is unchanged; its measurements remain in the raw evidence. The steady input
has two elements. Median times below are microseconds.

| Fixture | Preparation | First gradient | First row | Warm row |
| --- | ---: | ---: | ---: | ---: |
| Branch fill | 456.07 → 418.38 | 5.92 → 12.48 | 26.06 → 33.33 | 9.41 → 0.76 |
| Safe overwrite | 384.40 → 316.53 | 5.79 → 11.83 | 26.73 → 30.29 | 7.70 → 0.78 |

Warm rows improve 12.3× and 9.8×. Preparation improves enough to offset the
higher first-gradient and first-row costs in these phase medians. Process peak
RSS is roughly unchanged/slightly lower: 27.39 → 27.34 MiB for branch fills,
27.27 → 27.17 MiB for overwrites. These are whole-process peaks, not retained
executor memory measurements. No resource budget grows.

Complete inference uses separate fresh processes, 250 ms warmup and one-second
windows. Each invocation resets the seed and runs 100 warmup iterations,
100 draws, and their output generation. Timings exclude source, preparation,
and first-use costs. Values are median ± MAD in milliseconds.

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| gq_integer_fill_paths | 1.678 ± 0.003 | 0.329 ± 0.003 |
| gq_bounded_integer_refusals | 1.067 ± 0.007 | 0.307 ± 0.002 |
| ar1 | 7.888 ± 0.030 | 7.870 ± 0.037 |
| ode_nested_for | 68.328 ± 0.482 | 68.263 ± 0.319 |
| gq_container_rng_complete | 1.057 ± 0.006 | 1.062 ± 0.003 |
| gq_bounded_integer | 0.311 ± 0.000 | 0.311 ± 0.001 |
| gq_integer_fill_paths_refusals | 1.445 ± 0.010 | 1.458 ± 0.003 |

The target inference gains are 5.1× and 3.5×. Initial canary signals were
old-integer preparation 259.14 → 268.74 µs and rows 0.631 → 0.655 µs,
ODE preparation 399.01 → 419.90 µs, and MIR-canary inference
1445.21 → 1457.75 µs. One six-group counterbalanced baseline A/B and
candidate A/B confirmation followed. All primary and confirmation samples
are retained. Values below are group medians in microseconds.

| Confirmation | Baseline A / B | Candidate A / B |
| --- | ---: | ---: |
| Old integer preparation | 271.976 / 270.320 | 261.742 / 266.827 |
| Old integer row | 0.653 / 0.640 | 0.664 / 0.640 |
| ODE preparation | 409.042 / 419.384 | 401.200 / 404.772 |
| MIR inference | 1460.537 / 1457.571 | 1459.376 / 1463.824 |

The initial differences did not repeat consistently: preparation reversed,
old-integer row differences varied between 0 and 0.011 µs (group MADs
0.009–0.022 µs), and MIR inference differences changed sign between groups.
There is no resolved normal-use regression in these measurements. Finite
canaries cannot establish a universal no-regression guarantee.

Another six-pair run changes the parameter and length on every row,
alternating zero and the listed maximum. Median complete-row times in µs:

| Maximum length | Branch fill baseline → candidate | Overwrite baseline → candidate |
| --- | ---: | ---: |
| 1 | 7.24 → 0.88 | 7.35 → 0.86 |
| 2 | 7.89 → 0.90 | 8.19 → 0.94 |
| 32 | 28.40 → 1.62 | 18.96 → 1.78 |
| 128 | 94.28 → 4.11 | 51.11 → 4.24 |
| 512 | 370.13 → 361.75 | 195.16 → 192.88 |

Capacities 1–128 select the existing structured engine. Capacity 512 exceeds
the existing work budget and uses MIR in both builds; its timing difference
is not a compiled-path gain. The candidate library is 39,851,504 bytes,
1,184 bytes smaller than baseline, SHA-256
`8fe5ecd7ab18680e07312ece96548de29be3eec7620930588f228300ac5bb794`.
Compressed package size was not measured.

[Raw evidence](data/2026-09-29-complete-array-fills.json.gz) retains all 288
primary samples and 72 controls, inputs and hashes, binary/source identities,
build settings, runner protocols, selections, validation logs, and independent
references. The maintained benchmark manifest adds the branch-fill model and
a new still-interpreted refusal canary. Public corpus tables are unchanged;
exploratory runners remain under `.cache/`.
