# Remaining ctsem architecture work

Follow-up: [the proof packets](2026-09-20-ctsem-proof-packets.md) implement checked
layout reuse and recorded-executor relocation in `f70ba0f7`. The earlier clone
and layout deferrals below describe the state before those proofs. Parallel
replay and the Fréchet replacement remain deferred/rejected as documented.

Authorization: the user asked to execute the remaining promising parts of the
plan. The earlier numerical-kernel iteration is a checkpoint, not completion
of this broader work. This record tracks the remaining hypotheses through
experiments and implementation, rather than stopping after the next local win.

Fresh upstream: `680a4763b256764476bca72b7916ad312c910701`, fetched and integrated
without conflicts. The three intervening commits only change macOS CI. The
validated direct-forward change is preserved in checkpoint `22c2d86b`.
Existing baseline executables remain usable because runtime inputs and object
contents are unchanged; new variants are built and identified separately.

## Evaluator and continuation rules

Use the previous exact fixtures, four target sizes (32/33/400/4000), three
parameter points, changing-point/value-only sequence, pinned dependencies and
Release FP settings. Preserve the numerical policies: bitwise for unchanged
arithmetic; existing external/high-precision gates for deliberately changed
algorithms, with any internal allowance justified before judging a result.

Separate preparation, first gradient, warm forward/reverse, retained storage,
allocation and peak RSS. No full-inference claim without measuring sampling.
Full target decisions use six alternating fresh-process pairs. Run the exact
kernel/lifetime/refusal tests first, then target feasibility, ordinary controls,
full CTest and the 329-model three-point reference gate. Builds and correctness
jobs do not run alongside performance measurements.

Retain independent checkpoints and removable diffs for each hypothesis. A
fixed A/A and A/B confirmation resolves ambiguous signals without repeated
timing-until-success. An experiment's stop is a decision point; continue other
supported work instead of marking the entire roadmap complete. Only generic
family/activity/shape/control-flow/provenance predicates may enable changes.

## 1. Resolve QR retention's ordinary-model impact

Evidence: retaining plain-left factors is numerically correct and improves
the full target by about 17% together with direct forwards. Two unrelated
models still have positive timing signals; the direct-only ablation is neutral.
The earlier graph census found no solves in four other affected models.

Distinguish new executed work from compilation/link effects. Confirm the
remaining models' actual operation census and inspect their hot functions.
A same-binary scratch-disabled comparator isolates runtime retention cost;
it cannot by itself excuse a cross-binary regression.

Architectural experiment: separate the solve family into its own compilation
unit before adding retained factors, so changes to solve templates no longer
recompile unrelated matrix/density bodies. Measure separation alone and then
retention separately. Keep the exact original Eigen arithmetic and per-call
scratch lifetime; no padding or linker-order tuning. If this fails, inspect
actual shared function/code differences before choosing another variant.

## 2. Remove repeated recording/encoding work

Fresh first-gradient time is about 15 seconds. Historical PR392 profiles split
that cost between recording instructions and frame sealing; the next probe
must sample the current first-recording phase and quantify repeated metadata
construction separately from numerical work and required value movement.

The candidate is direct reuse/emission of a proven frame layout, retaining
fresh values and dynamic guards. A different counterproposal is a compact
recording instruction/binding program that avoids repeated operand lookup
while leaving frame encoding unchanged. Select the smallest discriminating
experiment from the profile. Cover changed branches, aliases, dynamic shapes,
exceptions/retry, partial writes and independent copied executors.

## 3. Audit recorded-model reuse and subject parallelism

Read actual sampler/pool clone order and kernel-state ownership. Measure an
unrecorded clone and a recorded source before proposing reuse. Sharing immutable
programs must not share mutable values, adjoints, guards or input pointers.
If the normal sampler clones before recording, include the cost and semantics
of moving the recording boundary rather than assuming a recorded source.

For subject parallelism, establish whether a generic independence/reset proof
can partition the actual retained computation. Evaluate current reduce_sum
retained-loop restrictions and the smallest fixture that could lift them.
Parameter/value reductions and exceptions must keep a stated numerical and
ordering contract. Unknown independence is a refusal, not permission to
recognize ctsem source or subject-variable names. Record concrete blockers
and a feasibility result where a proof-backed implementation is available.

## 4. Evaluate the remaining numerical alternative

Matrix-exponential derivatives account for about 4.4% of current warmed time,
so their target ceiling is modest but measured. After the larger opportunities,
assess a dedicated Frechet derivative against the doubled-size block method
on the observed sizes and an unrelated exponential consumer. Use the existing
high-precision oracle and adversarial non-normal matrices. Do not combine this
arithmetic change with factor retention before each is independently measured.

## Execution decisions

| Plan item | Disposition | Evidence and remaining prerequisite |
| --- | --- | --- |
| Isolate solves and retain plain-left QR | Implemented | Exact arithmetic and lifetime gates pass; warm target improves 13–15% beyond direct forwards. Ordinary timing uncertainty is recorded in the scorecard. |
| Reuse recording buffers and combine version metadata | Implemented | No frame-layout assumption; temporary storage released before replay. |
| Reduce recording dispatch | Implemented | Profile-guided fusion of proven data calls with adjacent branches; independent branch entries retained. |
| Emit frames from an established layout | Implemented in the proof follow-up | Every use certifies operand bindings, geometry, literal values and reverse order; mismatch uses canonical encoding. N4000 first-gradient time improves another 9.4%. |
| Reuse a recorded clone | Implemented for completed frame tapes | Transactional relocation keeps mutable storage private. Recorded copy plus first gradient improves about 25×; changing the normal sampler’s unrecorded-source boundary remains deferred, with no sampling-speed claim. |
| Automatically parallelize subjects | Measured and deferred | Actual frame dependencies couple 3,992 of 3,994 boundaries. A generic reset/independence proof and private workspace/adjoint reductions are prerequisites. |
| Dedicated exponential Fréchet derivative | Prototyped and rejected for general replacement | Faster microcases, but worsened oracle accuracy on non-normal inputs under the unchanged gate. |

The selected implementation, numerical validation, ordinary-control
confirmation and the review's additional branch-entry tests are complete.
The [performance scorecard](../../../notes/performance/2026-09-20-ctsem-remaining-architecture.md)
records the final selection and its limits. Raw artifacts live under
`.cache/ctsem-remaining/`; earlier experiments remain under
`.cache/ctsem-post-392/` and their durable scorecard.

## Audit and first experiments

The completed [Fable audit](2026-09-20-ctsem-remaining-fable-review.md) was read
and reconciled against source. CLI initialization and every assistant response
identify `claude-fable-5-1`; the CLI also reports ancillary Haiku usage. No tool
permission was denied. The audit made no edits or benchmark runs.

The new first-gradient sample contains 7,661 samples, all in recording:
`recording_forward` has 3,421 exclusive samples, including inlined helpers;
`seal_frame` has 582 exclusive samples. These flat symbols cannot distinguish
all inlined costs. The measured first gradient was 15.53 s, with the previous
3,995 frames, 4 programs, 638,725 code words and 2,655,409 bindings unchanged.

The first recording candidate reuses only the temporary Stream vectors and
seal remap/range/version scratch. Every semantic field is cleared; undo values
are already moved into each sealed frame. Scratch is released at recording
completion or discarded with a failed tape. No layout-equality assumption or
arithmetic change is made. Version-record fusion is a separate possible
ablation, not bundled into this first experiment.

The clone audit confirms C API and CLI sampler clones precede initialization;
executor pools also clone an unrecorded prototype. A recorded-state clone
would still copy roughly 614 MB of mutable cells and relocate 2.66M bindings.
The direct clone/first-gradient measurements below establish the actual
current behavior before any production ownership change.

The saved MIR contains no reduce_sum. Both lowering nesting orders and the
stateless import-compaction contract currently reject retained child loops.
Changing reduce_sum alone therefore cannot accelerate this target. Inspect
cross-frame value and adjoint references before considering automatic runs.

The exponential feasibility probe implements differentiated Padé approximants
and squaring from [Al-Mohy and Higham, Algorithm 6.4](https://eprints.maths.manchester.ac.uk/1218/),
with the derivative-specific norm thresholds. It lives outside production.
Keep the existing 1e-14 native/nested and 1e-9 external gates; compare both
methods against the existing 60-digit block oracle, including non-normal and
nilpotent matrices and many squarings. Any failure requires accuracy analysis,
not a relaxed threshold. Actual target extents will be counted before selecting
microbenchmarks. Nonfinite inputs would retain the current production fallback;
a probe alone does not authorize a production replacement.

## Decisions supported by the remaining probes

- QR isolation changes no unrelated matrix/density object bytes: SHA-256 of
  `matrix_fns.o` is `4d1a1bca9ed9e5bfa322fba73d12506043a0ea7cbe650779c3d41ad2b65490a7`
  for split-only, retained QR and both recording candidates. The actual
  `lotka_volterra` and `s2_mv_subset` graphs contain zero solves. This excludes
  new solve work and recompilation of those bodies, but does not prove the
  precise cause of every small cross-binary timing difference.
- The fixed QR confirmation did not establish a repeatable ordinary regression:
  all three raw A/B intervals include parity. `s2_mv_subset` remains uncertain:
  median 1.0238, raw interval [0.9935, 1.0646], A/A-adjusted interval
  [0.999985, 1.0715]. Do not label this universal performance parity or retry
  until the sign changes. Carry it into the final scorecard.
- Startup-only recording probes: buffer reuse reduced N400 first-gradient
  time 1.5648 to 1.5401 s (two pairs); fused version records then reduced
  1.5496 to 1.5192 s (two pairs). Warm timing stayed near parity. These are
  feasibility probes, not the final six-pair conclusion. Actual Version size
  is now 24 bytes versus 21 bytes across the previous three pools; automatic
  frame admission uses this actual size and may select frames earlier.
- Current clones are cheap (N4000: 0.193 ms from a cold source, 0.412 ms from a
  recorded one), but each clone records again: first gradient 15.205/15.085 s.
  Recorded source plus recorded clone reaches about 3.01 GB RSS. This confirms
  the startup opportunity, but the normal sampler has no recorded source and
  a relocation/deep-copy mechanism would not share mutable numerical storage.
  Defer that ownership change; no sampling acceleration is claimed.
- The diagnostic reference closure sees 395/3,995 frames at N400/N4000, no
  unknown references, and only two conservative runs. At N4000, 433,132 value
  bindings and 433,132 adjoint bindings target another non-prefix frame;
  maximum distance is one, coupling 3,992 frame boundaries. Shared workspace
  and prefix writes still need a separate proof. This does not expose thousands
  of independent subject groups; do not implement parallel replay from it.
- Matrix exponentials execute at extent 10, 2,001 calls per gradient at N4000.
  The standalone Fréchet probe is 36–46% faster on four extent-10 microcases,
  but fails the unchanged accuracy gate on non-normal matrices. For a 5x5
  Jordan-like case with off-diagonal 100, oracle scale error rises from
  2.38e-15 to 1.60e-13. Keep the established production block method. The
  prototype and all 47 high-precision cases are retained for future research.

The line-aware recording profile resolves much of the previous opaque symbol:
7,694 samples include 705 at the instruction-loop condition, 656 at its switch,
313 at data-kernel operand rebinding, and 656 in the comparison kernel. This
supported a bounded dispatcher experiment: fuse a proven data-only call
with its immediately following branch without changing either operation.
The isolated probe remains under `.cache/ctsem-remaining/branch-probe`; the
validated selection includes the resulting change.


## Implementation review and final validation

[Fable's implementation review](2026-09-20-ctsem-remaining-implementation-review.md)
found no correctness or lifetime defects. Its requested test gaps are closed:
the solve lifetime fixture now exercises top-level Executor calls, generated
island adjoints, and a looping island that repeatedly changes the divisor and
uses private var replay arenas. All compare bitwise to recomputation across
value-only calls and copied executors. The test passes.

Recording scratch after an exception is released when the unready tape is
reset on the next forward, or when the executor is destroyed; completion
releases it immediately. That matches the existing frame-tape failure lifetime.
The moved triangular solve includes an explicit zero-row return equivalent to
the previous empty result. Neither changes a numerical tolerance.

The final dispatcher probe has 444 eligible adjacent pairs in the target and
cuts N400 first-gradient time from 1.5118 to 1.4590 s in two pairs. It preserves
the 851 proven data kernels, 318 published values, 246 index sites, 99 prepared
indices and 33 prepared iterators, and every frame/program/code/binding count.
The selected candidate includes this fusion; the original branch instruction
remains a valid independent jump target. Existing control-flow/retry/order
fixtures pass, with a new assertion that only the eligible immutable producer
fuses. [Fable's independent delta review](2026-09-20-ctsem-remaining-dispatch-review.md)
found no actionable bugs and identified two additional direct-entry fixtures.
Both fixtures now pass: a sibling Branch entered before and after publication,
and a WhileTest entered via continue in the condition block, with exact
comparisons against unspecialized recording and tree evaluation.

On the complete selected implementation, all four sizes and three points are
bitwise equal to the established implementation. The largest target external
scaled difference is 3.2102e-13. Changing points 0/1/2/0 with value-only calls in
between also remain bitwise. Target modes are explicit: N32 and N33 use streams;
N400 and N4000 use frames, for both baseline and selected. Version-record size
changes the general admission estimate, but not these four observed modes.

All 329 reference models pass all three points (1,020,194 values, worst scaled
error 9.38e-13 against 1e-9). Full CTest passes 262/264; the two failures are the
same density/builtin signature model-generation baseline failures already
reproduced before this work. No test or reference exception was added.

Final six-pair target measurements reduce N4000 warm gradients from 474.438
to 411.757 ms (median paired ratio 0.8650, 95% interval 0.8624–0.8854), and
first gradient from 15.224 to 14.018 s (ratio 0.9218, interval 0.9101–0.9307).
The isolated recording contrast has unchanged live allocation and neutral
warm time; QR retention adds 32,833,600 live bytes. Small stream cases do not
establish a first-gradient improvement.

The selected ordinary controls and one fixed A/A–A/B follow-up do not confirm
a repeatable slowdown. Small effects remain uncertain, including the earlier
QR-only subset signal. The 322-case screen has 319 bitwise finite matches and
three matching nonfinite points; its timing is exploratory. Detailed intervals,
individual observations and all deferred/rejected experiments are retained in
the scorecard. No sampling-speed claim is made.
