# Bounded nested real arrays in output blocks

Follow-up: the [refusal-cost fix](2026-09-29-bounded-refusal-cost.md) removes
the measured oversized-case setup penalty with an earlier resource proof.
The results below retain the original #416 head's measurements and limitations.

## Decision and scope

Extend the existing structured output-block path to rectangular real arrays
with one bounded, changing outer dimension and fixed inner dimensions. This
closes another whole-output MIR fallback without adding an engine, runtime
kernel, feature flag, or storage budget. The native comparisons below determine
whether the extension is worth retaining; deleting MIR is not the objective.

Baseline is #415, merged as `bd118b2d85ba56df4e0372e07bceef6379168651`.
The task integrated that main revision without conflicts. The matching baseline
library is SHA-256
`8fe5ecd7ab18680e07312ece96548de29be3eec7620930588f228300ac5bb794`,
39,851,504 bytes. The initial 8-by-3 probe selected MIR and took 29.49 µs
per output row while alternating lengths zero and eight. That single probe
established feasibility headroom; the paired results below are the comparison.

## What the proof permits

For example, an output block can declare `array[n, width] real a`, where `n`
changes with the draw but has a proved finite range and `width` is fixed data.
It can fill `a[i, j]`, read individual elements, sum `a[i]`, and use `size(a)`
or `num_elements(a)`. Only scalar values cross the block boundary.

- Follow MIR-hoisted temporaries in the existing dependence analysis and reject
  known changing inner dimensions before constructing a trial graph. The isolated lowering checks locally introduced dimensions
  again. This avoids known unsuccessful speculation on unsupported inputs.
- Allocate the checked product of the maximum outer extent and fixed inner
  extents. Negative sizes, overflowing products and existing storage/work limits
  still refuse. Inner dimensions must be evaluable fixed data; a runtime
  singleton range alone is insufficient to erase their reads or checks.
- Snapshot the outer extent at declaration time, independently of later writes
  to the source integer. Carry that slot alongside the complete fixed-capacity
  shape, marking inner dimensions static.
- Reuse the existing multidimensional index kernel for single-index reads and
  writes. It validates each logical extent, including when another dimension
  makes the result empty. Full indexing yields a scalar; a partially indexed
  inner array is admitted only as a sum argument. No general slices or container
  assignments enter the new grammar.
- A fully selected outer entry has fixed inner shape and no capacity padding.
  Its one-dimensional sum uses the existing real-array reduction order.
  `size(a)` reads the outer extent; `num_elements(a)` multiplies it by the
  checked fixed inner width, with a proved int32 result range. Loop-range
  analysis and expression lowering share that shape proof.
- Initial NaNs, branches, writes, exceptions and surrounding RNG continuation
  retain their execution order. Refused trials leave the complete compiled
  prefix unchanged. No exception triggers a retry through MIR.

The bounds remain 256 slots, 8192 total slot elements, 4096 statement visits,
and 65536 loop-weighted elements. They are admission limits, not an RSS bound.
Existing one-dimensional paths and non-output retained-loop admission retain
their policies. A single container map records ranks without adding a second
allocation per name to the admission scan.

Specializing every possible outer size or rewriting arrays into a new flattened
representation was deferred: the existing typed index operation already has
the required geometry and validation contract. This change needs only lowering
and admission proofs. Both are directly exercised by enabled fixtures.

## Correctness and coverage

- Native CTest: 324/324 pass. Two-, three- and five-dimensional fixtures exercise
  direct and packed selector inputs, dimensions zero/one, changing draws,
  declaration snapshots, repeated fills, partial initialization, cancellation,
  scalar reads/writes, inner-array sums and `num_elements` loop bounds.
- Full-row comparisons against forced MIR are bitwise and compare RNG
  continuation after successful rows and exceptions. Refusals compare the full
  printed graph, including fills and output views, with structured admission off.
- Three independent CmdStan references cover density, gradients and complete
  seeded rows at three parameter points: 54 values, maximum 1 ULP. Ninety
  additional boundary runs cover empty shapes, uninitialized values, cancellation
  and bounds errors; 248 successful output values agree exactly (including
  non-finite classification), and rejection outcomes agree.
- The pinned CmdStan oracle remains 2.40 at `d3d5df6`; exact source and dependency
  provenance is retained in the reference artifacts. No tolerance was widened.
- Recorded corpus: 329/329 models, 1,020,194 values; the prior 7040-ULP
  cancellation exception remains within its existing scaled-error gate. This
  is not a new claim of universal 10-ULP agreement.
- All 373 previously inventoried selections match the #415 baseline. Ninety-five
  targeted selection reports include the new shapes and deliberate refusals.
  In the timed two-/three-dimensional inputs, maximum outer lengths 1–32 compile;
  128 exceeds the existing work budget and keeps MIR.

The maintained execution benchmark manifest adds the two- and three-dimensional
cases and a still-interpreted inner-dimension canary. The public model corpus and
its timing tables are unchanged. Five-dimensional packing is a regression
fixture, not another advertised benchmark workload.

## Native performance

Six alternating fresh-process pairs use native Darwin arm64 Release,
AppleClang 21, threads enabled, full LP, and Python/ctypes entry points.
Phase windows are 200 ms warmup / 250 ms measurement. Source compilation
is unchanged; its measurements remain in the raw evidence. Steady output
inputs have outer length two, inner width three, and an additional fixed
dimension of two in the three-dimensional case. Median times below are µs.

| Fixture | Preparation | First gradient | First row | Warm row |
| --- | ---: | ---: | ---: | ---: |
| Two dimensions | 307.45 → 341.08 | 5.77 → 14.58 | 51.06 → 42.96 | 29.79 → 1.28 |
| Three dimensions | 358.71 → 380.99 | 5.90 → 11.08 | 80.71 → 37.52 | 53.48 → 1.97 |

Warm output rows improve 23.3× and 27.1×. The two-dimensional case adds
about 34 µs across preparation/first-gradient/first-row phase medians;
approximately two additional rows recover that cost. The three-dimensional
case already recovers setup in its first-row phase accounting. These are
approximate phase sums, not an instrumented cold latency claim. The separate
first-use inference measurement below checks the complete boundary.

Two-dimensional process peak RSS is 26.95 → 27.07 MiB.
Three-dimensional process peak RSS is 26.93 → 27.15 MiB.
These are whole-process peaks, not retained executor memory.

Complete inference uses separate fresh processes, 250 ms warmup and one-second
windows. Each invocation resets the seed and runs 100 warmup iterations,
100 draws, and their output generation. Timings exclude source, preparation,
and first-use costs. Values are median ± MAD in milliseconds.

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| gq_bounded_nested | 7.223 ± 0.051 | 0.454 ± 0.002 |
| gq_bounded_nested3 | 13.847 ± 0.033 | 0.675 ± 0.004 |
| ar1 | 7.938 ± 0.045 | 7.913 ± 0.058 |
| ode_nested_for | 67.987 ± 0.174 | 68.021 ± 0.164 |
| gq_bounded_integer | 0.314 ± 0.002 | 0.313 ± 0.001 |
| gq_integer_fill_paths | 0.336 ± 0.003 | 0.337 ± 0.002 |
| gq_bounded_nested_refusals | 0.709 ± 0.000 | 0.699 ± 0.006 |

Initial short canary signals led to one six-group counterbalanced baseline A/B
and candidate A/B confirmation. All primary and confirmation samples are retained.
Values below are group medians in µs.

| Confirmation | Baseline A / B | Candidate A / B |
| --- | ---: | ---: |
| Ordinary graph row | 0.426 / 0.453 | 0.435 / 0.436 |
| Prior integer preparation | 274.953 / 265.847 | 266.005 / 261.845 |
| MIR preparation | 383.162 / 389.712 | 380.916 / 381.756 |
| MIR row | 4.686 / 4.592 | 4.798 / 4.609 |
| MIR inference | 704.928 / 703.698 | 700.045 / 701.258 |

The initial candidate repeatedly increased preparation on unsupported changing
inner dimensions (baseline control medians 376.884/367.858 µs versus
411.050/398.152 µs). A first early check was ineffective because MIR hoists
size expressions into local temporaries; diagnostics still showed a rejected
trial. Its timing run stopped after 227 samples. The final check uses existing
dependence propagation, follows those temporaries, and eliminates that trial:
preparation interpreter constructions fall from three to two for the targeted
refusals. No unused early-check code remains.

Final controls above remove the resolved preparation regression. Ordinary-row
and prior-integer signals do not repeat consistently. The MIR row comparison
varies across groups; complete MIR inference shows no loss. These finite canaries
do not establish a universal no-regression guarantee.

A further six-pair comparison changes parameters and alternates outer length
between zero and the maximum on every output row. Inner width is three, with
an additional dimension of two for the three-dimensional fixture. Median µs:

| Maximum outer length | Two dimensions | Three dimensions |
| --- | ---: | ---: |
| 1 | 15.65 → 1.04 | 22.97 → 1.34 |
| 2 | 20.41 → 1.15 | 33.82 → 1.64 |
| 8 | 49.03 → 1.83 | 100.09 → 3.44 |
| 32 | 176.39 → 5.11 | 354.36 → 13.17 |
| 128 | 673.64 → 677.43 | 1407.61 → 1382.65 |
| 2, inner width zero | 12.86 → 1.02 | 14.76 → 1.05 |

Length 128 keeps MIR in both builds; its timing difference is not a compiled-path
gain. Empty inner arrays still preserve the outer shape and its bounds checks.
No resource budget grows.

Budget-refused length-128 cases still pay for a failed trial. Their cold
preparation plus first empty row costs 832.90 → 997.31 µs (two dimensions)
and 769.77 → 920.71 µs (three dimensions). This roughly 0.15–0.16 ms setup
regression remains; warm output timings alone would hide it.

A separate six-pair first-use run includes preparation from existing MIR,
sampling (100 warmup/100 draws), all output rows and destruction. Library
loading, MIR/data file reads and source compilation are outside this boundary.
Values are median ± MAD in ms:

| Model | Baseline | Candidate |
| --- | ---: | ---: |
| Two-dimensional target | 8.030 ± 0.269 | 1.237 ± 0.040 |
| Three-dimensional target | 14.773 ± 0.915 | 1.454 ± 0.015 |
| Two-dimensional length-128 refusal | 84.460 ± 2.021 | 89.300 ± 0.237 |
| Three-dimensional length-128 refusal | 179.380 ± 6.770 | 180.190 ± 6.807 |

The compiled targets improve first-use inference by 6.5× and 10.2×. The
initial two-dimensional refusal signal exceeds its setup cost, so one fixed
six-group A/A B/B confirmation followed. Baseline groups were
85.450 ± 5.053 / 83.158 ± 2.867 ms; candidate groups were
85.944 ± 4.440 / 86.211 ± 3.514 ms. These noisy cold totals remain
inconclusive beyond the known setup regression. The three-dimensional totals
also do not resolve a change. No further sign-seeking repetitions were run.

Retain the coverage gain under the accepted normal-end-to-end policy, with
this explicit limitation: larger budget refusals do extra preparation and
have no compiled-path benefit. Their cold-inference results do not prove
non-regression. Improving their admission cost or raising coverage beyond the
current work budget is a separate follow-up; no budget was loosened here.

The candidate library is 39,852,848 bytes (+1,344 versus baseline),
SHA-256 `94718952288c0f9dc6a0020830423771f0163eec2b2bb8af01f09240f222f82e`.
Compressed package size was not measured.

[Raw evidence](data/2026-09-29-bounded-nested-arrays.json.gz) retains all 312
final primary samples and 96 final controls, plus the initial candidate's 312
primary samples and 96 controls, and 227 samples from the stopped ineffective
prefilter run, 48 first-use inference samples and 24 cold controls. It also
records input/source/binary identities, build settings, runner protocols,
selections, validation logs and independent boundary runs.
Exploratory scripts remain under `.cache/`; production contains only enabled
lowering and maintained regression fixtures.


## Remaining boundaries

Independently changing inner dimensions, arrays of vectors/matrices, nested
integer arrays, whole-container copies or escaping values, general slices,
effects inside the block and active callbacks remain outside this proof.
These are separate consumer, layout, initialization or differentiation
contracts; supporting this rectangular slice does not settle them. Broader
admission should follow a measured workload and an independent oracle.
