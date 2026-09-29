# ctsem memory and throughput: frames and recording digest

Historical September 19–20 work, starting from PR #391 (`25a1b02c`) and its
merge `0daf3b15`. Read [the loop guide](../../research/loops.md) for later
proof/recording work and remaining questions. Numbers below are historical
matched comparisons, not current-build predictions.

## Mechanisms and proof boundaries

1. Per-cell data provenance folded selected unchanged cells in mixed
   data/parameter containers. The ordinary kernel first validated the access;
   selectors and selected cells had to be proven constant. Changing guarded
   indices triggered recording again. Equal observed values never proved
   constancy. At 33 rows comparisons fell 87,630 → 22,425 and recorded
   instructions 481,546 → 258,780 in the initial experiment.
2. Incremental frames sealed completed iterations before the whole history
   grew. Stable adjoint identities were separated from recyclable live-value
   handles; in-place promotion, aliases, constants and undo state survived
   renumbering. Normalized programs were interned with full equality after
   hashing. Every row still executed during first recording.
3. A frame-gated linear recording program later reduced the first control-tree
   walk. It preserved kernel/check/recording order and left warm replay and
   ordinary preparation unchanged.

Frames were not a general static forward/reverse CFG compiler. Numerical history
still scaled with rows, irregular paths could need multiple programs, and code
sharing observed on ctsem did not prove bounded code for arbitrary control.
The earlier flat-frame and post-recording trace-interning experiments had not
solved this ownership/first-recording problem.

Historical automatic admission sampled eight trips and projected working
version storage: above 128 MiB and at least 128 instructions per sampled trip.
These were structural storage/work criteria, not model identifiers or permitted
regression thresholds. Segment-bearing plans refused frames. Current selector
constants must be checked in source before changing them.

## Final frame comparison against PR #391

Six alternating fresh-process pairs, Release/O3 with contraction disabled,
300 ms warmup and 1,000 ms measurement. Prep was measured separately; peak RSS
included preparation, recording and replay. All returned target values and
gradients matched the preserved baseline bitwise.

| Rows | Warm gradient ms, baseline / frames | First gradient s | Peak GB | MIR prep s |
| --- | --- | --- | --- | --- |
| 32 | 4.474 / 3.605 | 0.251 / 0.237 | 0.175 / 0.167 | 1.293 / 1.306 |
| 33 | 4.585 / 3.658 | 0.261 / 0.253 | 0.177 / 0.169 | 1.305 / 1.300 |
| 400 | 56.876 / 49.085 | 3.077 / 2.756 | 1.154 / 0.315 | 1.717 / 1.717 |
| 4000 | 587.009 / 498.870 | 31.595 / 26.966 | 10.425 / 1.522 | 1.376 / 1.377 |

At 4,000 rows: 15.0% less warm time and 85.4% less peak RSS; live post-gradient
allocations fell 5.360 → 1.408 GB. Frames were slower than the cell-proof-only
experiment, which retained far more memory. Do not multiply their separate
speedups. The 32-row prep signal did not repeat in a fixed 12-pair A/B and A/A
confirmation; exact preparation allocation counts were unchanged.

The separate recording-program comparison reduced first gradient at 4,000 rows
26.870 → 19.239 seconds (28.4%), while warm gradient was 500.223 versus
499.311 ms and peak RSS 1.5212 versus 1.5232 GB. At 400 rows first evaluation
improved 27.5%; small-case first-evaluation intervals included parity.
These compare against frames, not the original PR #391 baseline.

## Validation and unresolved signals

Tests covered aliases, duplicate writes, cross-frame promotion, changing
branches/indices/data, zero/one/many trips, while exits, slices, cloned/concurrent
executors, value-only calls, and retry after failure. A stale invariant memo
handle caused an ASan failure without the generation guard; the guarded version
passed targeted ASan/UBSan. Mixed-library container poisoning was disabled,
so this was not a claim of an entirely instrumented dependency stack.

The recorded corpus replay passed 329/329 models and 1,020,194 values. Historical
CTest was 260/262 with two pre-existing signature-manifest failures; later
branch test totals must not be projected backward. External scaled-error
agreement does not establish a universal ten-ULP bound.

[Ordinary-model controls](../../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md)
retain unresolved preparation/RSS observations, including a small process-RSS
shift despite equal model allocations. The user deferred that investigation;
there is no universal zero-regression conclusion. Profiles identified remaining
recording dispatch, index validation, iterator binding and sealing costs, but
sample shares were not additive speedup ceilings. Preserve runtime validation
and error order when exploring them.

The full record pins binaries, commands, six-pair samples and local evidence
under `.cache/ctsem-next/`, `.cache/ctsem-frames/`, and `.cache/ctsem-first/`.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/docs/superpowers/plans/2026-09-19-ctsem-memory.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
