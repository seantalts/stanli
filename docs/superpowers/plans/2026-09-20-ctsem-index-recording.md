# Recording-site metadata experiment

Base: fetched `origin/HEAD`, `0daf3b15452d758f290afffe70a1cf8e4759310b`,
already contained in `codex/ctsem-next-perf`. Existing dirty work is preserved.
The user authorized implementation of the next recording optimization and
deferred the previous ordinary timing/RSS investigation, which is archived
under `notes/performance/`.

The baseline is the validated temporary recording program with data-only
instructions: first gradient 19.24 seconds at 4000 ctsem rows in its final
six-pair experiment. Sources, test binary, library, benchmark object and
executables are checkpointed under `.cache/ctsem-index/checkpoint/`, with
SHA-256 identities in `checkpoint-identities.json`. Fresh matched comparisons
will use those executables; the historical timing is motivation only.

## Evaluator and first decision

The latest first-recording sample attributes 17.6% exclusively to index
validation and 7.8% to iterator binding. First collect an isolated execution
census of index descriptor shapes, dynamic counts/extents, and data versus
recorded calls. The scalar vector-index fast path already exists. The census
is diagnostic instrumentation, not a performance comparison.

The first implementation hypothesis is to cache immutable index geometry
inside the temporary recording program, starting with canonical data-only
reads having fixed descriptors and slot lengths. First execution must use
the established validation order and publish cached facts only on success.
Later executions must refresh live selectors and retain all dynamic checks;
untaken sites cannot throw during plan construction. Cached state dies at
the end of recording or on failure. No ordinary preparation pass or retained
per-row storage is permitted. Unsupported cases keep the existing path.

Counterproposal: eliminate repeated selection geometry by recording a
selector-to-position computation once and sharing it among consumers.
This could also remove duplicate validation in retained gather/cell-proof
consumers, whereas per-site metadata only removes fixed setup. It requires
proven selector lifetimes and mutation invalidation. The census separates
data-only reads from retained/update uses before choosing that larger
mechanism. A complete register/JIT backend remains a larger untested
alternative; its code-generation and preparation costs are not justified
by this bounded validation hotspot alone.

The first stopping artifact is a shape census plus a correct two-pair
N400 feasibility comparison against the saved baseline and disabled path.
Continue if it removes measured work with a first-gradient benefit and no
retained-allocation increase; revise or park a losing prototype instead of
enabling it. That checkpoint does not establish integration readiness.

Positive and adversarial fixtures must compare bitwise values/gradients and
exact exception behavior, cover changing selectors, empty selections, late
first use, guard changes, invalid descriptors, custom overrides and retry,
and positively assert that caching runs. Reuse the unrelated mixed-cell
canary. Integration includes target sizes 32/33/400/4000, phase/memory
measurements, ordinary controls, default/forced reference corpora, relevant
sanitizers, full configured CTest and formatting. New timing uncertainty
receives a fixed bounded confirmation rather than indefinite resampling.

Iterator bookkeeping is a subsequent independent experiment, selected from
the new profile after the index change.

## Census and first prototype

The isolated N400 compiled-recording census observed 30,056,774 index calls
at 389 sites. Of these, 18,443,410 (61.4%) are data-only rank-two point reads
with fixed extents/counts; another 215,825 are data-only rank-one point reads.
The remaining large groups include dynamic-extent retained reads and matrix
updates. This supports starting with fixed point reads instead of a general
mutable index-runtime cache or shared selection-position cache. Counts
measure opportunity, not time shares.

The v1 experiment specializes canonical data-only point reads of any rank,
where every axis is Single with count one and fixed extent. The existing
instruction publication bit also records that the normal index kernel has
successfully validated this site. Subsequent executions check every current
selector in the original axis order and compute the scalar offset using
the descriptor's already-validated strides. No instruction widening, extra
metadata allocation, retained cache, or cached data pointer is needed.

The proof depends on immutable descriptors and fixed KernelCtx lengths
(dynamic-length operations are already excluded by DataKernel eligibility).
Full validation on first use establishes legal geometry and offsets. For
fixed Single axes, the logical strides equal the descriptor's physical
strides; each later in-range selector produces the same exact integer dot
product as the general selector walk. The validated capacity bounds that
sum and prevents overflow. An invalid selector still throws before writing
the output. First-use failures and untaken sites retain the original path.
The opt-in prototype flag is `STANLI_STRUCTURED_PREPARED_INDEX=1`.

The two alternating N400 feasibility pairs measure baseline first calls at
1.960/1.969 seconds, the new binary with index specialization disabled at
1.935/1.958, and enabled at 1.630/1.635. All 581 returned values match PR391
bitwise. The integrated tests add canonical positive cases at ranks 1/2/3/9,
packed and direct inputs, array/matrix layouts, alias rebinding, first use
after frame eight, parameter guards, value-only calls, dynamic-geometry and
custom-kernel refusal, malformed descriptors in untaken arms, later invalid
indices and retry. The full structured-loop suite passes.

The new ten-second N4000 sample contains 7,717 main-thread samples, all in
first recording. Exclusive index validation falls to 345 samples (4.5%);
iterator binding is 651 (8.4%) and the recording dispatcher is 2,971 (38.5%).
Sealing including callees is 1,771 (22.9%). These remain interval sample
shares, not whole-phase timers or additive savings predictions.

This supports one isolated iterator experiment before final measurements.
For a transient loop iterator whose unique writer is its own For node and
whose bounds are proven constant conditional on existing guards, ForEnter
still performs the original binding. Subsequent iterations need only write
the new value to its stable workspace cell. The whole-region writer check
refuses aliases, kernel writes and reused iterator slots, and parameter-bound
loops retain the original path. Frame compaction preserves this external
workspace address and remaps the binding/version handles together. The
prototype lives in `.cache/ctsem-index/iterator/`, is opt-in through
`STANLI_STRUCTURED_PREPARED_ITERATOR=1`, and receives the same two-pair N400
probe against the index-only implementation before any integration decision.

## Iterator result and final validation protocol

The two alternating N400 pairs measured index-only first gradients at
1.646/1.633 seconds, the iterator binary with specialization disabled at
1.647/1.638, and enabled at 1.577/1.563. All 581 returned values match PR391
bitwise; warmed gradients range from 48.3 to 48.7 milliseconds. This supports
integrating the bounded ForNext specialization. ForEnter remains unchanged.

The integrated structured-loop suite passes. New iterator tests cover
changing and negative data bounds, empty loops, parameter-selected data
bounds, break/continue, and refusal for alias/kernel writes, reused iterator
slots, parameter-valued bounds and retained iterators. They assert positive
admission and bitwise equality to both the disabled specialization and the
original tree evaluator. The index and iterator paths are independently
disabled with `STANLI_STRUCTURED_PREPARED_INDEX=0` and
`STANLI_STRUCTURED_PREPARED_ITERATOR=0`.

Final runtime sources are frozen for measurement; identities are in
`.cache/ctsem-index/final-identities.json`. The matched baseline is the
pre-experiment recording program, not PR391 or the index-only intermediate.
Both phase executables link the identical benchmark object, use equal-length
paths and single-thread settings. The fixed protocol is six alternating
fresh-process pairs per ctsem size (32/33/400/4000), with 300 ms warmup and
1000 ms measurement plus separate fresh preparation. Seven preselected
ordinary controls receive the same six pairs. A broader 319-case sweep uses
two alternating pairs, 100 ms warmup and 300 ms measurement. These timing
series run serially without concurrent builds or tests. Live bytes/counts
must match at prepared, first-gradient and warmed boundaries, and outputs
must match bitwise. Timing and process RSS are reported separately from
live allocation; the previously deferred residual investigation stays
deferred. Any new material signal receives at most one fixed confirmation
before being reported or investigated structurally.

Correctness gates are full configured CTest, default and forced-frame
CmdStan-reference replay, all four ctsem sizes at three parameter points,
and targeted ASan/UBSan coverage of the recording and new refusal tests.
The final sampling profile is collected separately from timing comparisons.

The semantic gates pass: default and forced-frame replay each cover 329
models at three points and 1,020,194 values, with worst scaled reference
error 9.38e-13 against the existing 1e-9 gate. All four ctsem sizes at all
three parameter points match PR391's 581 density/gradient values bitwise.
Targeted ASan/UBSan reports zero failures, including the new tests; as in
the prior run, unchanged kernel objects are not instrumented and libc++
container poisoning is disabled for mixed instrumentation. Full CTest passes
260/262. The same existing density/builtin signature-manifest freshness
checks fail, with no new failing tests. Formatting and `git diff --check`
pass. The archive comparison confirms only `structured_loop.cpp.o` and the
archive symbol index differ; all other runtime objects are byte-identical.

Commands and logs are under `.cache/ctsem-index/`: `final-build.log`,
`final-structured-tests.log`, `ctest.log`, `corpus-reference{,-forced}.log`,
`numerics/results.json` and `asan/commands.json`. Reference replay uses
`python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check
--jobs 2`, with `STANLI_STRUCTURED_FRAMES=1` for the forced run.

## Final target measurements

The fixed six-pair comparison gives the following medians. All outputs are
bitwise equal to PR391; live bytes and allocation counts match exactly at
every paired phase boundary.

| Rows | First gradient, baseline | First gradient, candidate | Warm gradient, baseline / candidate | Preparation, baseline / candidate |
| --- | --- | --- | --- | --- |
| 32 | 0.2377 s | 0.2344 s | 3.609 / 3.591 ms | 1.2933 / 1.3029 s |
| 33 | 0.2548 s | 0.2476 s | 3.679 / 3.621 ms | 1.3017 / 1.3112 s |
| 400 | 1.9628 s | 1.5690 s | 49.114 / 48.983 ms | 1.7228 / 1.7294 s |
| 4000 | 19.3665 s | 15.2726 s | 497.360 / 497.699 ms | 1.3848 / 1.3908 s |

The first-gradient reduction is 20.1% at 400 rows and 21.1% at 4000 rows.
Paired log-ratio 95% intervals are 0.79269–0.80358 and 0.78327–0.79090,
respectively. These descriptive intervals use Student's t with five degrees
of freedom; full ranges and all medians are in
`.cache/ctsem-index/phases-target/summary.json`, alongside raw process logs
and a manifest identifying inputs and binaries. Warm-gradient, preparation
and peak-RSS intervals include parity at all four sizes. At 4000 rows, median
peak RSS is 1.522917 / 1.520542 GB; live storage after the first gradient is
exactly 1,408,224,672 bytes in both revisions. This experiment establishes an
additional first-recording speedup over the prior compiled recorder; it is
separate from the historical PR391-to-frames comparison.

All seven preselected ordinary controls have bitwise-equal outputs and exact
live-allocation parity. Timing intervals include parity except for the
mixed-cell canary's warmed gradient (18.118 / 18.566 microseconds). The
`s2_gev` peak-RSS interval also excludes parity (6,766,592 / 6,832,128 bytes),
while its timing intervals include parity. These two selected metrics
receive one fixed confirmation: six alternating A/B and six identical-binary
A/A pairs each, with 300/1000 ms windows and three fresh preparations per
label. The selection is fixed in `confirmation/selection.json` before those
runs. The broader two-pair sweep remains exploratory and does not launch
another open-ended ordinary-model investigation.

The completed broad sweep reproduces the same three nonfinite benchmark
points (`dogs_log`, `s2_invgaussian`, `sir`) in both binaries. All 316 finite
cases match bitwise. Across cases, the median warmed-gradient ratio is
0.99721 and geometric mean is 0.99909; corresponding preparation ratios are
1.00446 and 1.00434. These aggregate short-run observations cannot establish
the absence of every per-model regression. First-gradient and warmed live
bytes/counts match in every case. Preparation also matches except for one
`ch12_m12_5` candidate process: the allocator reports 29,684,320 bytes instead
of 32,813,664 with the same 50,969 blocks. Its other candidate preparation
and all gradient boundaries match. One separately fixed six-A/B/six-A/A
preparation-only check, three fresh processes per label, investigates this
lower reading without treating it as a memory improvement.

## Final decision and remaining limits

Keep both specializations: independent probes and the final matched
experiment establish a first-recording benefit with bitwise parity, safe
fallbacks and no retained-allocation growth. The genericity audit checks
descriptor geometry, provenance, writer identity and storage class only;
no model/source identifiers or benchmark-size predicates were added.

The fixed confirmations are complete. The mixed-cell canary's A/B warmed
interval now includes parity (0.96332–1.01815), as does its A/A-adjusted
interval (0.94358–1.02105). `s2_gev` retains a small RSS signal: medians
6,733,824 / 6,848,512 bytes, exactly equal live storage, A/B interval
1.01131–1.02029 and A/A-adjusted interval 1.00763–1.02318. Its cause remains
unresolved. The separate preparation control reproduces both `ch12_m12_5`
byte readings in baseline, candidate and A/A processes, with the same A/B
distribution in both revisions (3 lower / 15 higher readings each).
The new results do not establish universal ordinary-model process-RSS parity.

The final N4000 sample contains 7,698 main-thread samples entirely in first
recording: the recorder has 3,426 exclusive samples (44.5%), general index
validation 374 (4.9%), and iterator binding only 15 exclusive / 32 including
callees. Sealing has 1,900 samples including callees (24.7%). The recorder's
exclusive share includes inlined specialized operations, not just dispatch.
The next bounded experiment should separate operand lookup/dispatch from
frame encoding; direct frame emission remains an untested alternative that
needs a lifetime and variable-shape proof. No further change is included.

[Durable research notes](../../../notes/performance/2026-09-20-ctsem-recording-sites.md)
and their linked JSON retain the exact statistics, all case summaries,
confirmation protocols, profile counts and source/binary identities. Raw
measurements remain in `.cache/ctsem-index/`. The previous ordinary-model
investigation stays deferred; no padding, link-order tuning or adaptive
extra rounds are applied.
