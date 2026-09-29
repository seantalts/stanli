# ctsem memory and throughput follow-up

Initial base: PR391, `25a1b02cd417f98d0164ecc65bc6d6df160c0a9b`.
On the user's subsequent request to sync main, fetched origin again and
fast-forwarded to `0daf3b15452d758f290afffe70a1cf8e4759310b` (PR391's merge).
`origin/HEAD` points to `origin/main` and is an ancestor of HEAD. Both commits
have tree `2c131f77e4bfb0ccd9e76085c5662076a05dff50`; the patch was preserved
without conflict. The user requests faster ctsem and lower memory without
slower gradients, more memory, or slower preparation on ordinary models.

Initial frame implementation, validated before the later recording-program
optimization: at 4000 rows, warmed gradient time is 15.0% lower
and process peak RSS is 85.4% lower than PR391. The ordinary-model controls
found no repeatable gradient, preparation or memory regression. Reference
replay passes 329/329 models; CTest passes 260/262 with the two pre-existing
signature-manifest failures. The final scorecard and uncertainty are below.

## Evaluator

- Same Release build (`-O3`, `-ffp-contract=off`), retained baseline binaries
  and separate fresh processes. Inputs and raw results live in
  `.cache/ctsem-next/`. Preparation from MIR, first evaluation/recording,
  warm gradients, forward time, and peak RSS are distinct measurements.
- ctsem: exact prefixes of the retained issue data at 32, 33, 400, 4000 rows;
  use the transformed MIR that the prior retained-loop work measured. The
  current vectorized MIR refuses a logical-shape change and subsequently
  fails with `break outside a loop`; keep that existing failure visible.
- Internal numerical gate: bitwise density and all gradients, multiple
  deterministic points, repeated execution, changing branches, aliases,
  and mutable indices. External gate: existing CmdStan corpus references
  at their documented tolerance, and ctsem when its reference is available.
- Ordinary-model gate: six alternating baseline/candidate rounds with A/A
  control, including prep and process peak RSS; all 319 performance cases,
  with unsupported fixed-point cases reported explicitly.
  Timing noise is uncertainty, never an acceptable regression budget.
- Genericity: only operation semantics and execution provenance may control
  eligibility. Add an unrelated structural canary and adversarial fixtures;
  do not recognize model names, variable names, or source fingerprints.
- Full configured CTest and reference replay before accepting a change.

## Hypotheses and first decision

1. **Recording storage:** data-only intermediates still acquire retained
   versions and arena allocations before freeze removes them. At 33 rows the
   recorder creates 1,405,529 versions and 1,905,037 arena cells; freeze keeps
   549,247 cells. Measure which allocations are avoidable before changing
   ownership/lifetimes. Reusing storage must preserve aliases, historical
   backward reads, and immutable recorded operands.
2. **Runtime activity:** compile-time activity can overstate the derivative
   activity of an executed version. Measure unnecessary adjoints/backward
   records. Refuse pruning when a current active input can contribute.
3. **Counterproposal — retain less executable work:** track data provenance
   through selected container elements, so reads of unchanged data cells in
   mixed data/parameter containers fold during recording. This would remove
   whole instructions and guards instead of shrinking their records. The
   smallest experiment is a loop that updates one active cell and branches
   on a disjoint data cell; a parameter-controlled overlapping write must
   invalidate the proof. Measure opportunity before committing to a new
   per-element provenance representation.

The census selected hypothesis 3. At 33 rows, 87,630 replayed calls were
comparisons, despite the preceding data folding. Tracking unchanged cells
through copy-on-write indexed updates reduced comparisons to 22,425 and the
whole stream from 481,546 to 258,780 instructions. A data-cell read folds
only after the normal kernel has validated it and every selector and selected
cell is proven constant. Parameter-selected reads remain live. Varying write
indices retain their existing guard, so a changed selection re-records before
using dependent constants. There is no numerical-value equality heuristic.

The initial cell-proof implementation was confined to `structured_loop.cpp`.
It made no changes
to the selector, lowering, preparation passes, ordinary executor, kernel
layout, or replay dispatch. Metadata is owned by the recording state and
released before arena compaction. `STANLI_NO_STRUCTURED_CELL_CONSTANTS=1`
ablates the proof; `STANLI_NO_STRUCTURED_REPLAY=1` remains the tree oracle.

Hypothesis 1 remained open after that experiment: per-iteration version
records still accounted for substantial recording memory. Recycling those
records requires separating
the stream's adjoint identities from live value handles and respecting
in-place adjoint promotion. The frame implementation below makes that
ownership change. Hypothesis 2 remains deferred rather than combined with
this ablation.

## Earlier cell-proof experiment

- ctsem at 33 and 400 rows, points 0, 1, 2: all 581 returned numbers
  bitwise identical to PR391. Fresh CmdStan reference: maximum scaled error
  `5.18e-15`, well inside the established external `1e-9` gate. Raw values
  and build command are in `.cache/ctsem-next/numeric-*` and
  `cmdstan-ref-command.json`.
- Full reference replay: 329/329 models at all three points, 1,020,194
  numbers compared. Maximum scaled error `9.38e-13`. This includes the
  language fixtures selected by `verify_refs.py`; the performance inventory
  selects 319 application cases.
- CTest: 260/262 pass. The two failures are the existing stale builtin and
  density signature manifests, also reported by PR390. The source changes
  do not touch either generator or manifest.
- New tests cover data-import changes, data/active cell overlap, aliases,
  snapshots read after overwrites, duplicate writes, multi-index/range reads,
  parameter-selected reads and writes, branch flips, and forward-only calls
  before gradients. They compare bitwise with both ablated replay and the
  tree walk. An independent mixed-cell recurrence exercises automatic loop
  selection as well as the one-iteration boundary.
- Performance protocol: six fresh-process alternating pairs; 200 ms warmup
  and 250 ms measured windows, following the repository timer. Prep uses
  a separate `--prep` process and RSS comes from `/usr/bin/time -l`. Binaries,
  input hashes, commands, samples, stdout/stderr, and settings are retained
  under `.cache/ctsem-next/{target,corpus,aa}`. An initial partial target run
  overlapped the end of CTest and was discarded in full; it is retained as
  `target-discarded-contention` and excluded from the results.

These results precede the frame implementation. The target and seven-model
A/A runs completed before the sync. The ordinary
corpus sweep was stopped to address the architectural question, with nine
completed case records retained. It does not establish the ordinary-model
performance gate. The suite totals above also preceded the last guarded-
constant fixture and strengthened bitwise assertions; the final-build
validation below includes those additions.

Completed target results (medians of six fresh processes per binary; GB is
decimal process peak RSS, not steady-state storage):

| Rows | Gradient ms, base / candidate | Peak GB, base / candidate | MIR prep s, base / candidate |
| --- | --- | --- | --- |
| 33 | 4.672 / 3.766 | 0.177 / 0.170 | 1.296 / 1.297 |
| 400 | 55.500 / 45.358 | 1.154 / 0.952 | 1.731 / 1.726 |
| 4000 | 586.152 / 458.028 | 10.426 / 8.772 | 1.393 / 1.393 |

At 4000 rows the gradient ranges are 580.332–597.465 ms and
449.506–462.100 ms; peak ranges are 10.424–10.426 GB and 8.772–8.772 GB.
This is a 21.9% reduction in median gradient time and a 15.9% reduction in
median peak RSS. All target comparisons passed the driver's bitwise gate.
The independent mixed-cell recurrence's gradient medians are 19.781 / 18.658
microseconds; peak medians are 6.398 / 6.447 MB. The small memory increase
required attribution; it was not classified as a passing no-regression
result. The final phase measurements below resolve the canary's memory and
prep signals. These earlier measurements concern the per-cell proof alone.

## Lessons from PR390/391

PR390 already folds data-only calls, compacts arenas and pointer pools,
interns index arrays, guards changing control/indices, and provides
`STANLI_NO_STRUCTURED_REPLAY` as its semantic oracle. Closed-form matrix
adjoints also removed substantial arithmetic cost. PR391 repaired full
environment/model copies during write-array speculation and restored the
island-first path, which had doubled HMM prep. Avoid adding shared lowering
analysis or broader retained-loop selection to this experiment.

## Architectural proposal: shared loop code and numerical frames

The following section records the proposal that preceded implementation;
the implementation and its limits are recorded below. The question is whether the runtime can
keep the loop's code proportional to the body and its storage proportional
to the numerical state needed for differentiation, without building the
full per-execution instruction stream first.

There is direct prior evidence against a naive implementation:

- Commit `2821987d` fingerprinted complete recorded iterations. Exact
  fingerprints gave one template per ctsem row. Removing position and guard
  values gave five instruction shapes at 33, 400 and 4000 rows. Thus exact
  trace interning has no demonstrated sharing; shape interning alone still
  leaves per-row operand and adjoint tables.
- Commit `9af3160b` examined those shapes. Of the gather sites, 9,386 had
  identical position arrays and two required a constant shift; for in-place
  sites, 6,492 were identical and two required a shift. No other case appeared
  in these historical measurements. This motivates symbolic addresses, but
  observed similarity is not a proof for arbitrary models or parameters.
- Commit `5783f845` removed the diagnostics after the template experiment
  was rejected. Reintroducing post-recording interning alone would also keep
  the expensive first walk and its peak allocations.
- The earlier static-storage design already explored numerical frames and
  generated segment adjoints. Its flat-loop preconditions excluded nested
  loops and in-place updates, both important here. Current ctsem diagnostics
  report zero segments. Generalizing the relevant ownership and control
  contracts is necessary; simply enabling the earlier path is insufficient.

The current 400-row baseline census tracks 494.1 MB of frozen storage, of
which 122.3 MB is numerical arena, adjoints, undo values and target workspace.
The remaining 371.8 MB includes instruction records, pointer pools, range
maps and other metadata. Recording also creates 15,804,073 version entries
and makes 389,352,856 node visits. These are measured opportunities, not a
predicted memory floor or speedup.

### Representation

1. **Reusable code.** Lower an eligible retained body to shared blocks with
   explicit branches and loop edges. Bind operands as invariant inputs,
   offsets in the current numerical frame, carry locations, or offsets in
   data tables. A row index or base offset is a binding, never an absolute
   pointer copied into every instruction. Irregular positions remain data
   tables when a simpler symbolic form cannot be proven.
2. **Data execution and provenance.** Preserve data-only facts per output
   and, where provable, per selected container region. Specialize pure data
   computations and geometry once for unchanged inputs. A compact schedule
   may select different shared blocks across rows. Every row-varying decision
   that affects specialization must be represented; a representative row or
   matching parameter values do not establish equivalence. Validation,
   exceptions and effects constrain what can be moved or skipped.
3. **Numerical frames.** Each execution saves values that backward kernels
   actually read, overwritten cells needed for undo, and required control
   decisions. Use the existing `BackwardPrimalReads` contracts, retaining
   conservatively when unavailable. Keep immutable data outside frames and
   recycle temporary workspace. Fixed-shape/count loops can plan strides;
   variable paths need checked frame growth. Peak state remains O(N) when
   N iterations' numerical history is required.
4. **Stable derivative identities.** Separate the small table of live value
   bindings from frame-based historical adjoint identities. Reuse live
   handles when dead instead of retaining a version for every transient
   result. Aliases, in-place promotion, cross-iteration carries and duplicate
   indexed writes must preserve the existing reverse accumulation order.
5. **Shared reverse code.** Run the existing kernel pullbacks in reverse
   execution order over the frames, including saved branches and undo data.
   Hold arithmetic, shared-parameter accumulation and target reduction order
   fixed in the first experiment. Generated scalar adjoints and machine-code
   dispatch are separate later experiments, if profiling justifies them.

The intended storage is shared code plus invariant inputs, compact control
and bindings, required numerical frames, and reusable workspace. Work must
become compact before full execution is expanded. Compacting afterwards
cannot remove the first-recording cost already paid.

### Protecting ordinary models

Enter through the existing retained-loop selector. Keep all new analysis
and storage inside selected regions; add no whole-model pass, executor field
or speculative model/environment copy. Construct any additional execution
plan lazily for a selected region and measure that work as first-evaluation
cost, separately from prep. Failed eligibility must restore all trial state
and use the established path. PR391's environment journal and island-first
write-array behavior are constraints on the integration.

Structural isolation limits exposure but does not establish zero regression:
the 319-case gradient/prep/RSS comparisons and unrelated retained-loop
canaries remain acceptance gates. A changed branch, shape or index must
execute correctly or fall back before stale state can be consumed.

### Smallest useful experiment and alternatives

First build an independently switchable prototype for a fixed-shape counted
recurrence with mixed data/parameter containers, row-varying data indices,
aliases and indexed updates. Keep arithmetic kernels unchanged. Assert that
code size and live binding metadata stay bounded as rows increase, while
saved numerical frames grow by their accounted stride. Compare bitwise at
multiple points and after branch/index changes; unsupported control must
refuse transactionally. Then inspect whether the same contracts cover the
actual ctsem body, starting at the selector boundary and scaling to 4000.
The historical five-shape result is a coverage hypothesis to verify, not a
specialization key or permission to recognize this model.

Counterproposals remain independently measurable: recycle recording version
metadata while keeping the current stream (smaller change, still retains
the expanded instruction history); generate machine code for dispatch
(does not inherently remove that history); checkpoint/recompute numerical
frames (can reduce numerical memory but spends execution time). The first
shared-code prototype should use full required-state retention so code
sharing and recomputation are not confounded. At the proposal stage, no
speed, memory or prep claim for this architecture had been established.

## Implementation: incremental frames

The user authorized implementation after reviewing the proposal. The branch
was fetched and verified again at `0daf3b15`. The earlier cell-proof patch,
fixtures and binaries were checkpointed in `.cache/ctsem-frames/` before
editing. Initial runtime and binary identities are in
`validated-identities.json`; the final identities are in
`final-identities.json` there.

The first implementation changes when recording is compacted. It seals the
prefix and each completed outer iteration, preserving only numerical ranges
referenced by the program or still-live bindings. It compacts the version
table at each boundary. Backward operands use stable adjoint offsets; an
owned inactive container that is promoted in a later frame has a separate
promotion identity shared by its earlier consumers. Constants and alias
ownership survive handle renumbering.

Each frame is encoded with local offsets and external value/adjoint bindings.
Gather and update positions are relative to their first selected position,
so a row offset becomes a binding. Exact normalized programs are interned
with full equality after hashing; hashes or observed numerical equality
never establish eligibility. All rows execute during recording. Guards keep
their existing replay-and-respecialize behavior, and kernel calls, target
reduction and reverse accumulation keep their order.

This is an incremental compiler for recorded iterations, not yet a static
forward/backward CFG compiler. It removes the full-loop recording history
and shares code before later rows are recorded. It still visits the original
control tree on the first evaluation. Shapes without reusable programs may
retain multiple programs; code-size independence is measured for the target
and canary, not asserted for arbitrary control flow.

Automatic selection uses the existing eight-trip sample and projects the
working version bytes (value/adjoint pair, owner and constant flag). Above
128 MiB, with at least 128 recorded instructions per sampled trip, it seals
that prefix and uses frames for subsequent trips. The replay-work floor
rejects mostly folded or tiny bodies that would pay frame dispatch without
enough code to share. This is
a storage policy, not a performance-regression allowance. Smaller recordings
keep the previous stream, with no new lowering pass or environment copies.
`STANLI_STRUCTURED_FRAMES=0` disables frames; `=1` forces eligible plans from
their first iteration, including outer while loops. Segment-bearing plans
are excluded before execution. The independent cell-proof ablation remains
available.

The focused tests cover code-size and working-version bounds as row count
grows, zero/one/many trips, data changes, parameter branch and selector
changes, aliases, duplicate writes, cross-frame promotion, target ordering,
while exits, dynamic slices, copied/concurrent executors and forward-only
calls. A failure injected after automatic selection verifies that a retry
discards the partial frame tape. A zero-length import exposed a dangling
zero-length pointer in the existing freeze checker; freeze now stores null
for that unused import pointer.

Initial probes, not the final repeated timing scorecard:

- Forced frames at 33 and 400 rows: seven programs and 463,299 code words
  at both sizes; at most 96,190 working versions. Both runs are bitwise
  identical to the earlier cell-proof binary.
- Forced frames at 4000: the same seven programs and working-version peak;
  1.521 GB process peak RSS and 497.756 ms per gradient in one process. All
  581 density/gradient numbers match PR391 bitwise. This is substantially
  less memory, but slower than the cell-proof-only warmed gradient; the
  architecture's current dispatch cost is a measured remaining opportunity.
- CTest: 260/262, with only the two existing signature-manifest failures.
  Forced-frame corpus reference replay: 329/329 at all three points,
  1,020,194 values, maximum scaled error `9.38e-13`.

Validation runs and raw artifacts are under `.cache/ctsem-frames/`:
`numerics/` checks four ctsem sizes and three parameter points;
`benchmarks/target/` and `benchmarks/corpus/` record six alternating
fresh-process pairs, separate preparation and process peak RSS. The new
driver pins input and binary hashes and preserves every command/result.
The ordinary-model gate also includes the fixed confirmation and phase
attribution runs documented below.

### Repeated frame measurements

The initial automatic-frame binary (`09430e6b27fee508e230fa326fc9787c21be353e7bdef7c74d3057f53e3e0b90`)
completed six alternating pairs against the PR391 baseline
(`0a2c76ff7915eac57c584e11e11fc4bacf31964cc8f0caa7396678aa138466e3`).
These measurements precede the additional replay-work floor and stale-memo
fix. The final rebuild and affected validation are documented below.

| Rows | Gradient ms, PR391 / frames | Process peak GB, PR391 / frames | MIR prep s, PR391 / frames |
| --- | --- | --- | --- |
| 32 | 4.399 / 3.683 | 0.175 / 0.168 | 1.318 / 1.314 |
| 33 | 4.695 / 3.554 | 0.177 / 0.170 | 1.312 / 1.308 |
| 400 | 57.007 / 48.663 | 1.154 / 0.318 | 1.737 / 1.740 |
| 4000 | 587.784 / 502.791 | 10.426 / 1.520 | 1.393 / 1.388 |

At 4000 rows this is 14.5% less gradient time and 85.4% less process peak RSS.
The peak includes recording and replay. Whole gradient-process wall medians
are 34.748 / 30.027 seconds; this is not an isolated first-evaluation timer.
The independent mixed-cell canary is 19.951 / 19.021 microseconds per gradient,
with identical median peak RSS of 6.365 MB and preparation 0.747 / 0.759 ms.
The small positive prep differences were subjected to the same noise
controls as the ordinary corpus, recorded below; they were not treated as
an allowable regression budget.

All four ctsem sizes at points 0, 1 and 2 are bitwise identical to PR391 in
all 581 density/gradient values. Fresh full-size CmdStan comparisons give
maximum scaled errors of `4.13e-13`, `3.28e-14` and `7.21e-15`, respectively.

The ordinary sweep triages any median gradient or RSS increase above 3%,
prep increase above 5%, or smaller effect with a positive paired 95% lower
confidence bound. These are investigation triggers, not permitted losses.
The confirmation set is fixed before its results are observed: six
counterbalanced A/B and identical-binary A/A pairs, 300 ms warmup and 700 ms
measurement, with three fresh prep processes per label per pair. Prep-only
flags repeat prep alone. Raw samples, selection criteria, and binary/input
hashes are saved under `benchmarks/`.

### Final-build correctness and eligibility

The final build adds the replay-work floor and rejects stale invariant memo
handles when a guard change starts a new recording. The regression fixture
retains invariant results alongside two generations of changing values, then
changes to an arm that does not execute those invariant sites. With the
generation check removed, AddressSanitizer reports a heap-buffer-overflow
in `seal_frame`; with it present, the frame suites pass ASan and UBSan.
The targeted sanitizer build instruments the changed runtime and test suites
and links unchanged kernels. Libc++ container poisoning is disabled because
mixed instrumented/uninstrumented template instances produce false reports;
ordinary address checks and heap redzones remain enabled. Commands and both
test outcomes are in `.cache/ctsem-frames/asan/`.

Final binary: `5df0d2eae6faa25eb4eb763e1b130f0e636d8bcb8cfbbb863aaf36d120920474`.
Source and check-binary hashes are in `final-identities.json`. This build
passes the structured-loop suite, the repository format check, and 329/329
default-path reference models at all three points (1,020,194 numbers, maximum
scaled error `9.38e-13`). Full CTest remains 260/262, with only the two
pre-existing signature-generation manifest failures.

The ordinary six-pair sweep completed with 316 finite cases, every returned
density and gradient bitwise identical to PR391. `dogs_log`, `s2_invgaussian`
and `sir` reject the fixed benchmark point as non-finite in both binaries;
they are not classified as performance passes. Corpus median ratios are
1.00047 for gradient time, 0.99840 for prep and 0.98699 for process peak RSS.
Individual results, rather than these aggregate medians, selected 69 cases
for fixed confirmation: 26 timing, 44 prep and 6 RSS flags, with overlap.

The final default-policy census selects frames in none of the 319 ordinary
cases. Only `s2_gev` executes an ordinary retained-loop stream. All flagged
cases execute outside the changed recorder and frame engine. This establishes
path isolation, not a substitute for the A/B and A/A measurements. Census
commands and diagnostics are retained in `benchmarks/final-census/`.

### Attribution of ordinary-model flags

The fixed 69-case confirmation completed without numerical failures. Five
signals remained positive after comparison with their local A/A controls:
RSS for `Mth_model` and `hmm_drive_0`, prep for `ch11_m_pois` and
`sw_categorical`, and gradient time for `sw_se`. They were investigated rather
than excused by the unchanged aggregate median or their execution paths.

An unchanged PR391 recorder was rebuilt and substituted into a copy of the
final runtime archive. All other objects and archive member order were held
fixed. The counterfactual used the actual `stanli_runtime_objects` compile
flags. Relinking unchanged source itself shifted process RSS. VM snapshots
showed allocator fragmentation, while snapshots taken during an active
gradient also caught different numbers of temporary allocations. Those
asynchronous snapshots could not establish a difference in model storage.

A phase driver therefore samples allocation statistics at completed gradient
boundaries. It uses one compiled driver object for both runtimes and identical
length executable paths, with probes outside the warmed timing window.
Twelve alternating fresh-process pairs were fixed for the five remaining
cases and the independent canary. Every density and gradient also had to
match the saved PR391 output bitwise. Commands, source, hashes and samples
are in `phase-driver/`, `relinked-baseline/` and `benchmarks/phases-ordinary/`.

For all five ordinary cases, **every pair has exactly equal live allocated
bytes and allocation counts** after prep, the first evaluation and warmed
evaluation. For example, warmed live storage is 1,339,168 bytes for
`Mth_model` and 6,703,840 for `hmm_drive_0` in both revisions. Their median
RSS ratios in this controlled run are 0.99075 and 0.97923. All five paired
95% intervals include parity for both gradients and preparation. The
independent canary uses 28,608 fewer live bytes after evaluation; its paired
gradient ratio interval is 0.92291–0.95389, with prep consistent with parity.
The remaining flags did not survive this controlled attribution step.

The allocator's `max_size_in_use` reports zero on this host and is treated
as unavailable. Process peak RSS still comes from the operating system;
live allocation counters explain it rather than replacing that metric.
These are measurements of this corpus, data and host, with explicit timing
uncertainty; they do not establish an exact zero-cost guarantee for every
possible model and machine.

### Final target measurements

The same phase driver was linked with the final runtime and the controlled
PR391 archive described above. Six alternating fresh-process pairs per
size used 300 ms warmup and 1000 ms measurement windows, with preparation
measured in a separate fresh process. First-gradient time excludes MIR
preparation. Process peak RSS includes preparation, recording and replay;
the phase readings and `/usr/bin/time -l` peaks agree. All returned density
and gradient numbers match the saved PR391 results bitwise.

| Rows | Warm gradient ms, PR391 / frames | First gradient s, PR391 / frames | Peak GB, PR391 / frames | MIR prep s, PR391 / frames |
| --- | --- | --- | --- | --- |
| 32 | 4.474 / 3.605 | 0.251 / 0.237 | 0.175 / 0.167 | 1.293 / 1.306 |
| 33 | 4.585 / 3.658 | 0.261 / 0.253 | 0.177 / 0.169 | 1.305 / 1.300 |
| 400 | 56.876 / 49.085 | 3.077 / 2.756 | 1.154 / 0.315 | 1.717 / 1.717 |
| 4000 | 587.009 / 498.870 | 31.595 / 26.966 | 10.425 / 1.522 | 1.376 / 1.377 |

At 4000 rows, warmed gradient time falls 15.0%, first-gradient time falls
14.7%, and peak RSS falls 85.4%. Baseline/candidate gradient ranges are
583.350–592.122 / 496.720–504.526 ms, with a paired 95% ratio interval
0.84384–0.85771. First-gradient ranges are 31.454–31.739 / 26.596–27.273 s;
peak ranges are 10.424–10.429 / 1.517–1.523 GB. Live allocations after the
gradient fall from 5.360 to 1.408 GB. Preparation has identical live bytes
and allocation counts in every pair at every size.

The 32-row prep median initially increased 1.02%, with a paired 95% ratio
interval 1.00218–1.01587. This triggered a fixed twelve-pair A/B and
identical-binary A/A prep-only confirmation, counterbalancing both pair and
block order. The confirmation medians are 1.293 / 1.297 seconds, with an
A/B interval 0.99594–1.00903 and an A/B-versus-A/A interval
0.99126–1.01568. Every run has exactly 27,480,992 live allocated bytes in
85,614 blocks. The initial prep signal did not repeat; no source changes
were made between these measurements. The other three sizes' initial prep
intervals include parity. All confirmation commands and results are in
`benchmarks/phases-target-prep-confirmation/`.

The first evaluation still interprets each row's control tree. Shared code
and bounded working versions remove most of the recording metadata cost,
but do not eliminate that walk. Numerical history still grows with row
count. Frame replay is also slower than the earlier cell-proof-only
experiment, which retained much more memory; the combined implementation
improves both gradient time and memory relative to PR391.

Raw commands, samples and input hashes are in
`.cache/ctsem-frames/benchmarks/phases-target/`. The identical phase driver
object links to baseline binary
`aebb5beed0f35ae21531ac4ea56c1b082a487f55f0308deabc9f798314d4cf1d`
and candidate binary
`db5e1d6d7736f04ed158106130cbe2cad6d27aefa8a2a2cebfad308ea0d05d9e`;
archive and source hashes are in `phase-driver/identities.json`.

## Follow-up: architectural review and first-gradient cost

The user requested a Fable architectural review through the `claude` CLI,
followed by implementation of the next step. On 2026-09-20, origin was
fetched again and remained at `0daf3b15`. The dirty source, tests, production
binaries, runtime archive and phase driver were preserved under
`.cache/ctsem-first/checkpoint/`; `checkpoint-identities.json` pins them.
Fable's assignment covers both the current change and future alternatives,
with read-only tools. Its exact prompt and response are retained in that
directory's parent.

The first discriminating measurement samples the unchanged first gradient
for ten seconds, starting three seconds after process launch. All 7,743
main-thread samples are inside the recording forward walk. Exclusive sample
counts include 3,300 in `Execution::forward` (42.6%), 1,071 in index
validation (13.8%), and 974 in transient-call execution (12.6%). The outer
recording loop attributes 1,126 samples (14.5%) to frame sealing, including
its callees. This is a sampled middle portion of recording, not a complete
phase timer or a benchmark. Raw stacks and commands are in
`first-gradient.sample.txt` and `profile-commands.json`.

Candidate mechanisms, before the review decision:

- Compile the existing control tree into a reusable linear recording
  program with explicit jumps. This removes recursive sequence/branch
  dispatch while keeping every executed kernel, check, and recording action.
  It can be constructed lazily only after frame admission, so ordinary
  preparation and warmed frame replay need no new work. Its ceiling is
  bounded by interpreter overhead; it does not eliminate frame encoding.
- Strengthen proven loop invariance or dependency caching to remove repeated
  pure data computations and index validation. This can remove more work,
  but must account for writes through aliases, input mutations, effects,
  validation order, and transient buffers whose identities stay fixed while
  their contents change. Equal observed values cannot establish invariance.
- Compile directly to frame-relative forward and reverse programs. This
  could remove both interpretation and per-iteration encoding, but requires
  a larger ownership, control-history and dynamic-shape proof. The measured
  encoding cost alone does not justify that rewrite without addressing the
  larger interpretation cost.

The implementation gate remains bitwise differential values/gradients,
unchanged validation/exception behavior, branch/index changes and retry after
failure, unrelated canaries, and the full applicable suites. Performance
compares the preserved frame baseline and the next candidate at 32, 33, 400
and 4000 rows, separating prep, first gradient, warmed gradients and peak
RSS. Ordinary-model signals require bounded controls; no timing threshold
is permission to accept a regression. The immediate stopping artifact is a
review decision and a correct, independently switchable feasibility result;
only a measured surviving mechanism proceeds to default integration.

### Fable review, decision and implementation

The initial `claude --model fable` invocation identified itself as
`claude-fable-5-1`. After the user's request for profile evidence and tool
access, the same session was resumed explicitly with
`--model claude-fable-5-1`, Read/Glob/Grep/Bash enabled, and the profile,
measurements and future alternatives supplied. Fable used its own inspection
tools. Its review and a focused follow-up on the data specialization are
saved in [the review record](../reviews/2026-09-20-ctsem-fable.md). Both CLI
initialization records confirm Fable 5.1; raw JSONL and prompts are under
`.cache/ctsem-first/`.

The review found no confirmed bug in the existing cell proof or frames. I
accepted the recommendation to measure data-only work and to build a
frame-gated recording program. I did not accept its performance estimates
as measurements. Call counts cannot establish time shares; removing frame
encoding alone does not bound an architecture that also removes interpreter
work. Its proposed clone reuse additionally needs relocation, lifetime,
thread-ownership and guard proofs, and a recorded source executor. It does
not automatically remove K minus one recordings in every sampling workflow.
The claim that collected live ranges are nearly sorted remains unverified:
collection groups ranges by use, not allocation order. Neither clone reuse
nor a sorting rewrite is included in this change.

An isolated counter build splits actual executed calls, excluding invariant
memo hits. All-inputs-constant calls are 97.036% at 33 rows and 96.944% at
400 rows. Constant transient calls alone are 86.447% and 86.339%. The two
runs remain bitwise identical to the saved PR391 values. Counter source,
commands and output are in `census/`; there is no counter overhead in the
production build.

The prototype ledger, all with two alternating N400 probes, is:

- v1 removes recursive control-tree dispatch but calls the old leaf
  dispatcher. First evaluation improves about 8%.
- v2 shares direct kernel/alias/target helpers. The compiled path improves,
  but the compiler outlines the kernel helper and makes the old fallback
  8–10% slower. Rejected in that form.
- v3 keeps the shared kernel helper inlined, restores the old path and cuts
  first evaluation from 2.714/2.715 to 2.287/2.255 seconds.
- v4 prebinds narrow contiguous data spans. It adds less than 1% beyond its
  flat-program control, so the extra span representation is discarded.
- v5 specializes individual proven data calls, including isolated calls
  separated by control. First evaluation is 1.967/1.971 seconds versus
  2.717/2.707 for the preserved frame baseline. All 581 returned values are
  bitwise equal and retained allocations are identical. These are feasibility
  probes, not the final timing result.

The integrated implementation consists of a temporary `RecordingProgram`
owned by the recording `Execution`. It lowers nested sequence/if/for/while
control into explicit jumps and per-loop cursors, preserving integer-bound
checks and the existing while-condition continue behavior. It uses the same
kernel, alias and target implementations as the tree path. Plans are built
lazily only after frame admission and die after completion or failure.
There is no new preparation pass, persisted cache or warmed-replay format.

A whole-region fixed point marks slots that any possible writer can make
nonconstant. Seeds are non-data-only imports; propagation covers kernel
outputs, aliases, in-place bases and loop iterators. Effects and noncanonical
forwards taint outputs. A specialized call must be a canonical, pure,
inactive transient, with no invariant memo, dynamic lengths, secondary
output, self-read or alternate output writer. It still reads current input
bindings and calls the same validating kernel on every execution. Its output
workspace is stable during recording. Publication happens only after its
first successful call; frame compaction remaps its unique output binding
and its site version together.

This proves constancy conditional on existing guards, not loop invariance.
A parameter branch can choose between two data constants because the branch
and its guard still execute in their original position. Nothing is hoisted
or omitted from control execution. Parameter writes anywhere in a container
conservatively prevent its static specialization; the existing per-cell
runtime proof remains available. The two independent ablations are
`STANLI_STRUCTURED_COMPILED_RECORDING=0` and
`STANLI_STRUCTURED_DATA_RECORDING=0`.

Tests exercise nested break/continue including continue in a while condition,
zero trips, invalid/nonfinite bounds, custom-kernel call order, guard changes,
input mutation, partial index failures and retry. Unmodified canonical
kernels plus positive diagnostics prove that data instructions really run.
Dedicated cases cover parameter-controlled constant assignments,
read-before-definition until iteration nine, interleaved value-only calls,
in-place data updates, and a later parameter write into another cell.
A variable-inner-shape/multiple-top-level-loop case pins correctness and
program diversity. The first-top-level-loop selection limitation remains:
a later large loop can still occupy one large frame. Distinct row shapes can
also grow the shared-program table. Neither case has a universal bounded
memory claim.

For clone reuse, inspection of `stanli_sample_multi_write_array` confirms that
it creates the chain clones before entering per-chain initialization and
sampling. Merely teaching the copy constructor to relocate an already-ready
tape therefore cannot guarantee a startup benefit when the source model has
not yet recorded one. A later design should measure actual API call order
and chain concurrency before choosing shared immutable programs, relocated
tapes, or an explicit recording phase.

### Integrated recording-program validation and target measurements

The Release build and structured-loop suite pass. Default and forced-frame
reference runs each pass all 329 models at all three points: 1,020,194 values,
worst scaled error 9.38e-13. The four ctsem sizes at three points each match
all 581 density/gradient values bitwise against the saved PR391 outputs.
The instrumented recorder and frame/control/data fixtures pass ASan and
UBSan. As before, unchanged kernel objects are not instrumented, and mixed
libc++ container poisoning is disabled; ordinary heap checks remain enabled.
Full CTest is 260/262, with the same pre-existing density/builtin signature
model-generation failures. Format and diff checks pass. Commands, logs and
source/binary identities are in `.cache/ctsem-first/`; no production source
changed during these measurements.

Six alternating fresh-process pairs compare the preserved frame implementation
with the new recorder, using the same phase-driver object and equal-length
binary paths. Warmup is 300 ms, measurement is 1000 ms, and preparation uses
a separate fresh process. Every pair has exactly equal live bytes and
allocation counts after preparation, first gradient and warmed evaluation.

| Rows | Warm gradient ms, frames / recorder | First gradient s, frames / recorder | Peak GB, frames / recorder | MIR prep s, frames / recorder |
| --- | --- | --- | --- | --- |
| 32 | 3.590 / 3.624 | 0.229 / 0.235 | 0.1666 / 0.1672 | 1.2965 / 1.2981 |
| 33 | 3.640 / 3.639 | 0.250 / 0.251 | 0.1689 / 0.1698 | 1.3022 / 1.3037 |
| 400 | 49.023 / 48.821 | 2.707 / 1.963 | 0.3168 / 0.3180 | 1.7232 / 1.7187 |
| 4000 | 500.223 / 499.311 | 26.870 / 19.239 | 1.5212 / 1.5232 | 1.3788 / 1.3836 |

At 400 and 4000 rows first evaluation improves 27.5% and 28.4%, with paired
95% ratio intervals 0.71446–0.73459 and 0.71073–0.72608. All gradient and
preparation intervals include parity, as do first-evaluation intervals for
32 and 33 rows. The 32-row peak-RSS interval narrowly excludes parity at
1.00005–1.00895 despite exactly equal model allocations; its fixed
12-pair A/B and identical-binary A/A confirmation is preserved in the
[research notes](../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md).
Other peak intervals
include parity. The linked executable's text segment grows one 16 KiB page;
its data segments and initializer table have unchanged sizes. Process RSS
must be assessed separately from live model storage.

The follow-up ten-second sample has 7,710 main-thread samples, all inside
first recording. Exclusive samples include 2,301 in the recording dispatcher
(29.8%), 1,354 in index validation (17.6%), 602 in iterator binding (7.8%), and
43 in the remaining generic transient path (0.6%, versus 12.6% before).
Frame sealing, including callees, accounts for 1,523 samples (19.8%). These
are sample shares from a middle interval, not complete phase times or
additive speedup ceilings. The next candidates are repeated iterator
bookkeeping and static index-descriptor validation. Dynamic bounds and
indices must still be checked in the original execution order; moving a
possible error out of an untaken branch would be wrong. Neither speculative
change is included here.

The ordinary-model controls and attribution experiments are archived in
[deferred research notes](../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md),
with the complete remaining case list and durable statistics. On 2026-09-20
the user deferred this investigation so ctsem work can continue. The results
are preserved as unresolved observations, not a claim of zero regression.
