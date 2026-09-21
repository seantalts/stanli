# Issue 374: scalar backward experiment

Benchmarking resumed at the user's request after a pause for another task's
measurements. The revised candidate passes correctness validation, makes GEV
gradients about 8% shorter and full inference about 7% shorter, and shrinks
the CLI and shared library by 16,512 bytes each. Ordinary-model live heap use
is unchanged. Follow-up memory maps show matching resident executable pages
and overlapping allocator occupancy. The evidence supports the candidate for
review, with short-CLI timing and peak-RSS variability retained as limitations;
it does not establish a universal absence of ordinary-model costs.

The [compressed evidence packet](2026-09-21-issue-374-scalar-evidence.json.gz)
retains both candidates, all paired results and controls, numerical outputs,
artifact identities, source patches, diagnostic helpers, and validation logs.
It is UTF-8 JSON compressed with gzip. Local raw outputs remain in the cache
directories named below.

## Starting point

The branch was fetched and verified current with `origin/HEAD` at
`45ea5cca1e4f583e57963438a0a3994841c5ab69` before this experiment. The previous
[matched CmdStan refresh](2026-09-21-issue-374-refresh.md) supplies the frozen
baseline; its binaries remain unchanged in `.cache/issue374-20260921/`.
The runtime still uses the same pinned Stan Math implementation.

## First candidate: separate single-output path

The first candidate bypassed argument/output handle buffers and the weighted
seed expression for a single output. It preserved mixed var/double overloads,
used `+=` for signed-zero behavior, and kept Stan's nested reverse sweep.

Two six-pair measurements found GEV baseline/candidate gradient ratios of
1.120 (MAD 0.025) and 1.099 (MAD 0.024). A four-seed CLI comparison found a
1.165 median paired wall-time ratio. All density, gradient, and output answers
at the 39 targeted points matched the frozen baseline byte for byte, and
all six sampled models reproduced the baseline's draw bytes for every seed.

However, Eight Schools showed roughly 2% longer warm gradients, including
in a 12-pair confirmation with one-second windows. Zero-inflated Laplace
showed roughly 3% longer full inference. The executable grew by 16,512 bytes,
and process-RSS measurements increased on ordinary models. Measurement noise
remains relevant, but these observations do not satisfy the ordinary-model
cost requirement. This version is not the active source change.

Raw plans, paired data, A/A controls, phase/inference results, profile, binary
hashes, and the exact patch are retained locally in
`.cache/issue374-scalar-20260921/`. Its `candidate.diff` and
`frozen-candidate.json` identify that version independently of the active code.
Sixty-seven runtime objects matched the baseline byte for byte; only
`scalar_binary.cpp.o` differed.

The first candidate passed the full recorded CmdStan replay: 329/329 models,
three points, 1,020,194 compared values. CTest passed 264/266 checks. The two
manifest-generation checks differ only in `stanc_build_id`: the local binary
reports `stanc3 v2.39.0-210-gd58446e`, while committed manifests say
`stanc3 v2.40.0`. Their signature hashes, cases, and all other generated content
match. The compiler source pin is the same as in the frozen baseline; this
environment-label mismatch was not hidden by changing committed manifests.

## Active candidate: direct seeding for all output sizes

The revised change keeps one generic implementation and removes the temporary
graph for `sum(y[i] * dout[i])`. It adds each upstream seed directly to the
corresponding Stan var's adjoint in **descending lane order**, then calls
Stan's own reverse sweep. Argument/output buffers and function calls retain
their original construction order. There is no separate scalar specialization.

The old weighted graph first deposits every seed in reverse lane order, then
runs the function callbacks. Direct seeding preserves that order. It matters
when selection functions return one shared broadcast var for several outputs;
ascending accumulation can change cancellation. `+=` also preserves zero-sign
handling. The numerical implementation of each function remains upstream Stan's.

The active diff touches `runtime/kernels/scalar_binary.cpp` and
`tests/test_scalar_binary.cpp`. Focused scalar-binary and MIR-binary tests pass.
Additional tests cover mixed activity, non-unit/non-finite seeds, signed zero,
aliased adjoints, rejection and outer-tape recovery, empty outputs, and
cancellation-sensitive reverse accumulation into shared broadcast outputs.

An initial six-pair comparison against the frozen baseline found a GEV
baseline/candidate gradient ratio of 1.087 (MAD 0.028). The two Laplace
fixtures showed no clear change. Eight Schools' paired ratio was 0.963
(MAD 0.026), compared with 0.975 (MAD 0.032) in its A/A control; that result
does not distinguish a change from measurement noise. Executables shrank
16,512 bytes and the runtime archive shrank 23,480 bytes. The frozen build,
plan, helper scripts, focused tests, and first measurements remain in
`.cache/issue374-seed-20260921/`.

## Current-main validation

The task branch was fetched and fast-forwarded to
`3c74330aa7fecb5db5e3262822a0f59476f9a167`, preserving the experiment.
The integrated changes add include support and benchmark-reporting updates;
they do not change this kernel. The new compiler integration was rebuilt in
an isolated stanc3 checkout at the exact required source pin. Its fetched
default branch is twelve commits ahead of that pin; the pinned source remains
the dependency contract. The stock compiler was built before applying the
Stanli overlay and correctly reports `stanc3 v2.40.0 (Unix)`. Shared dependency
artifacts were not modified.

The current-main control uses all the candidate's objects except a separately
compiled pristine `HEAD:runtime/kernels/scalar_binary.cpp`. All 68 other
runtime archive members match byte for byte, including member order. Both
executables use the same driver objects, embedded compiler, and link flags.
The executable and archive size reductions are unchanged on this base.
Plans, commands, compiler provenance, object/binary hashes, and raw results
are in `.cache/issue374-current-20260921/`.

All **266/266 CTest checks** pass, including the two manifest checks that
previously differed in their local compiler version label. The complete
recorded CmdStan replay passes **329/329 models**, three points and 1,020,194
values at the existing scaled-error gate. Its worst comparison is 9.38e-13
(7,040 ULP); passing this gate does not establish a uniform 10-ULP bound.

At all 39 targeted evaluation points, current baseline and candidate answers
match the frozen answers exactly through both source and MIR entry points,
including output names, values, and gradient lanes. Rerunning the independent
CmdStan reference executables also reproduces their frozen answers exactly.

The confirmation protocol was fixed before the run: twelve alternating A/B
pairs per model, 200 ms warmup and 1,000 ms measurement windows, all thirteen
models and twelve-pair GEV/Eight Schools A/A controls. Six models also receive
separate source/MIR preparation, first-gradient, full-inference, and memory
measurements. Timing runs are sequential and do not overlap this task's
builds, tests, or profiling.

## Current-main performance results

Ratios below are medians of within-pair **baseline/candidate** ratios; above
one favors the candidate. Parentheses contain MAD, not confidence intervals.
The separate time medians need not divide to the median paired ratio.

| Model | Gradient µs, baseline / candidate | Paired ratio (MAD) |
| --- | ---: | ---: |
| `eight_schools_noncentered` | 0.223 / 0.225 | 1.012 (0.021) |
| `i320_sratio_cs` | 78.365 / 79.112 | 0.991 (0.010) |
| `i320_sratio_plain` | 60.234 / 60.443 | 0.996 (0.013) |
| `s2_gev` | 7.007 / 6.444 | 1.087 (0.013) |
| `s2_mixture_theta` | 6.792 / 6.947 | 0.990 (0.014) |
| `s2_zi_asymlaplace` | 4.914 / 4.893 | 0.998 (0.009) |
| `sw_asymlaplace` | 4.563 / 4.554 | 1.011 (0.012) |
| `sw_cratio` | 59.132 / 58.773 | 1.003 (0.009) |
| `sw_cratio_cs` | 78.059 / 77.847 | 1.001 (0.007) |
| `sw_cumulative` | 22.287 / 22.199 | 1.003 (0.005) |
| `sw_cumulative_cs` | 51.325 / 50.009 | 1.021 (0.014) |
| `sw_re_negbin` | 2.172 / 2.153 | 1.012 (0.006) |
| `sw_sratio` | 59.971 / 59.319 | 1.006 (0.007) |

GEV's A/A ratio is 1.001 (MAD 0.012); Eight Schools' is 0.999 (0.009).
GEV's improvement repeats across both source bases and is well separated
from these controls. Most other changes are small relative to variability.
Their fixed-point gradient results do not establish unchanged full inference.

All six models reproduced the original 1,000-warmup/1,000-draw CSV bytes for
seeds 1–4 in both engines, excluding comments. Thus sampler work and the
previously recorded diagnostics are unchanged. The CLI timer includes source
preparation, generated quantities, and CSV formatting.

| Model | CLI ms, baseline / candidate | Paired wall ratio (MAD) | Peak RSS KiB, baseline / candidate |
| --- | ---: | ---: | ---: |
| GEV | 126.38 / 118.20 | 1.070 (0.023) | 16,864 / 16,888 |
| Asymmetric Laplace | 130.09 / 138.78 | 0.946 (0.024) | 16,672 / 16,720 |
| Zero-inflated asymmetric Laplace | 163.34 / 170.40 | 0.967 (0.052) | 17,600 / 17,696 |
| Eight Schools | 24.84 / 24.82 | 1.008 (0.002) | 14,192 / 14,248 |
| `i320_sratio_plain` | 1,032.26 / 1,045.47 | 0.988 (0.013) | 24,416 / 24,400 |
| `sw_cumulative` | 315.36 / 311.88 | 1.016 (0.028) | 15,616 / 15,648 |

GEV's sampling-phase ratio is 1.082 (MAD 0.024). Its first gradient from
source is 138.71 / 132.62 µs and total preparation 7.106 / 6.464 ms.
From MIR, preparation is 0.970 / 0.979 ms and the first gradient is
137.77 / 133.77 µs. Source-compilation medians are 6.241 / 5.663 ms;
the kernel change does not alter the compiler, so this cold-phase difference
should not be attributed to faster source compilation.

## Ordinary-model confirmation

The initial Laplace CLI results triggered a separate twelve-pair A/B and A/A
confirmation, with every seed repeated three times and comparison/engine
order alternated. All draw bytes still match. Asymmetric Laplace's wall ratio
is 0.974 (MAD 0.028), compared with 1.022 (0.040) for A/A. Zero-inflated
Laplace is 0.983 (0.068), compared with 0.987 (0.080) for A/A. These short
processes have substantial timing variation; the negative observations remain
in the evidence rather than being replaced by subsequent measurements.

A bounded longer-run diagnostic uses six pairs for each comparison, with
10,000 warmup iterations and 10,000 draws. All corresponding draw hashes
match. It helps distinguish sustained sampling cost from short-process
effects, but does not replace time-to-first-posterior measurements.

| Model | Longer CLI A/B ratio (MAD) | Longer CLI A/A ratio (MAD) |
| --- | ---: | ---: |
| Asymmetric Laplace | 1.003 (0.012) | 1.000 (0.004) |
| Zero-inflated asymmetric Laplace | 0.988 (0.008) | 0.991 (0.011) |

Retired instruction counts agree within roughly 0.01% in these comparisons.
The longer-run cycle ratios are 0.997 versus 0.999 A/A for asymmetric Laplace,
and 0.988 versus 0.987 A/A for zero-inflated Laplace. There is no clear
sustained slowdown separated from the controls. Short-CLI behavior remains
less certain; the longer workload does not prove it harmless.

Source preparation was also repeated with twelve A/B and A/A pairs for the
two controls whose first phase medians looked slower. Eight Schools' total
preparation ratio is 0.992 (MAD 0.010), versus 1.008 (0.031) for A/A;
`i320_sratio_plain` is 1.002 (0.006), versus 1.001 (0.004). The earlier
6% and 4% differences in separate medians do not repeat at that magnitude.
First-gradient timings are short and variable; their raw distributions are
retained, and no uniform first-gradient speedup is claimed.

## Memory and installed size

The CLI and shared library each shrink **16,512 bytes**. The runtime archive
shrinks **23,480 bytes**. A gzip-level-6 comparison shrinks the CLI by 4,333
bytes and the shared library by 1,984 bytes; these are compressed artifacts,
not complete release-package sizes. The shared library's exported symbol set
is unchanged. Both builds retain the same dependencies and installation path.

Separate memory probes sum live allocations across macOS malloc zones after
preparation, the first gradient, and warm execution. Across four paired trials
per source/MIR entry point, the five ordinary models' median live heap use is
unchanged. GEV uses 32 fewer bytes after warm execution. This measures live
malloc storage, including retained buffers; it is not an isolated measurement
of every executor allocation or of resident executable pages.

A direct `lmultiply` vector probe checks scalar and larger cases with the same
prepared input/output buffers and matching gradient hashes. Its post-warm
live heap readings include those buffers and all malloc zones:

| Output lanes | Baseline bytes | Candidate bytes | Reduction |
| ---: | ---: | ---: | ---: |
| 1 | 84,144 | 84,112 | 32 |
| 16 | 86,000 | 85,360 | 640 |
| 1,024 | 608,272 | 305,136 | 303,136 |
| 16,384 | 6,244,432 | 5,589,072 | 655,360 |

The savings vary with allocator capacity boundaries. They are a kernel
microbenchmark, not an observed whole-model memory reduction of that size.
The public Stan allocator's `bytes_allocated()` counter sums only through its
current block; after nested recovery it omits later retained blocks. Its raw
post-gradient readings are therefore not used as proof of retained memory.

Process RSS is a separate concern. The initial CLI table shows small increases
in five models. Zero-inflated Laplace's repeated paired RSS difference changes
sign, but asymmetric Laplace still has a median candidate increase of 40 KiB
in the short confirmation and 88 KiB in the long one. Its A/A median differences
are 0 and -8 KiB, with overlapping per-process ranges. Unchanged live heap and
smaller files do **not** prove unchanged RSS. Different resident code pages are
a possible explanation that the following snapshots do not support.

Three additional paired asymmetric-Laplace processes were stopped after two
seconds of a long sampling job to inspect their memory maps. These are
intentionally interrupted diagnostic runs, not completed sampling results.
Every process has **326 resident executable-text pages**, 5 resident
data-constant pages, 152 resident executable-data pages, and 5 stack pages
(16 KiB per page). The candidate's text mapping is one virtual page smaller.
Resident malloc-zone pages are `475, 468, 469` for baseline and
`470, 472, 470` for candidate: the same mean, with overlapping occupancy.
Both report approximately 6,083 KiB allocated in those zones and a median
physical footprint of 13.4 MiB. This locates variation in allocator page
occupancy without evidence of extra model storage or resident executable
pages in these snapshots. It does not reconstruct every completed run's peak.

## Decision and next bounded investigation

Retain this source patch and its semantic regression tests for review. It
establishes a reproducible GEV benefit with matching numerics, lower allocation
demand, and smaller artifacts. Longer ordinary-model inference and memory-map
controls do not show a clear sustained or retained-storage regression.
Short-CLI and peak-RSS differences remain in the record; repeat those on a
quieter, stable host when stronger acceptance evidence is needed. Avoid adding
model-specific dispatch or arbitrary code padding to tune one executable
layout. No further kernel change was made in response to timing noise.

Compact scalar loop execution remains a separate, unimplemented hypothesis.
The original GEV ULP discrepancy and the pathological mixture/negative-binomial
sampling diagnostics remain unresolved; neither candidate purports to fix them.
