# Fable 5.1 review of ctsem frames and first-gradient architecture

Requested by the user; produced through the `claude` CLI with model
`claude-fable-5-1` and Read, Glob, Grep and Bash inspection tools. This is
the reviewer's assessment, not independently verified performance evidence.
Inclusive profile shares overlap. Implementation decisions and experiments
are recorded in [the research log](../plans/2026-09-19-ctsem-memory.md).
The prompts and raw response are preserved under `.cache/ctsem-first/`.

Implementation follow-up: the temporary recording program and conservative
data instructions below were implemented and tested. Several recommendations
in the original review require qualification: call counts do not establish
time ceilings, the live-range collection order is not proven sorted, and
the sampling API clones executors before chain initialization, so copying
an already-recorded tape cannot promise K minus one recordings. A later
linker experiment also did not establish binary layout as the cause of
ordinary-model timing/RSS signals. The user deferred that investigation on
2026-09-20; its measurements and qualifications are preserved in
[research notes](../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md).

Review complete. Findings below, ordered by what should change what you do next.

## Headline: the first gradient is an interpreter running data-only bookkeeping

The diagnostics already on disk fix the shape of the problem. At 33 rows the recording makes 34,860,719 tree visits but produces only 481,546 stream instructions and 132,789 replayed calls, so about 98% of visits leave nothing behind (see `.cache/ctsem-next/census-33.diag`). Visits per row are flat, about 1.0M at both 33 and 400 rows, so at 4000 rows the recording walks roughly 4 billion nodes in the ~23 s the profile attributes to body interpretation. That is about 5.7 ns per visit, which is already close to an interpreter's floor. Eigen is 54 samples out of 7743.

Inclusive shares from the sample (10 s window, mid-recording, not a phase timer):

| Subtree | Share |
| --- | --- |
| run_transient (data-only and parameter compares) | 29.4% |
| index_forward, of which validate_index | 16.0% / 13.8% |
| seal_frame, of which sorting live ranges | 14.2% / 5.7% |
| run_retained + run_in_place | 10.0% |
| Execution::forward exclusive (inlined run) | 42.6% |

Consequence: every mechanism currently on the table is a per-visit optimization with a bounded ceiling. The flat control program can remove part of the 42.6% but keeps every leaf on the same substrate. Direct frame-relative lowering can remove at most the 14.2% seal share. Only executing data-only subtrees on a different substrate, with no versions, bindings, KernelCtx rebinding, or per-call descriptor validation, changes the constant.

## Confirmed facts about the current change

**No confirmed bug in the frame or cell-proof paths.** I traced the risky contracts by reading and each holds:

- Promotion identities: links are created only for live, owned, inactive versions at seal (`runtime/src/structured_frames.inc:242-248`), updated on in-place promotion (`runtime/src/structured_loop.cpp:1861-1866`), and a version promoted within its own frame resolves at seal time because the encoder reads the adjoint after the frame ran.
- An aborted replay leaves in-place mutations, but every in-place base is re-established by the import copy and the recorded Copy instruction on the next pass, so there is no persistent corruption. The stream path has the same property.
- Stale memo handles are nulled only when the site's generation is invalid (`structured_frames.inc:227`), which is the case the sanitizer run caught.
- Target sources are never Transient because a target use forces retention (`structured_loop.cpp:414-417`), so no target pointer can be a workspace cell.
- The cell proof is conditional on guards that precede its consumers in replay order. Write selectors are snapshotted, branch decisions are guarded, and data imports are compared on entry, so a folded read cannot outlive the facts it depends on.

**Confirmed architectural fact, not yet recorded anywhere: executor clones re-record.** The copy constructor calls bind_() (`runtime/src/executor.cpp:388-396`), which clears kernel states and calls make_state fresh (`executor.cpp:493-504`). Both clone_executors (`runtime/src/nuts.cpp:190-196`) and ExecutorPool copy-construct. So K chains pay K first gradients and K concurrent recording peaks. The copied-executor test only proves independence and exercises exactly this re-record. This is a separate lever from first-evaluation speed: relocating a ready FrameTape into a clone is O(tape) and bounded, and cross-chain reuse is safe by construction because data imports are guarded on entry.

**Architectural limits worth a fixture, not bugs:**

- find_outer picks the first top-level loop (`structured_loop.cpp:2878-2894`). A region whose root is Sequence{small For, big For} seals around the small loop and the big loop becomes one unbounded frame, so version_peak and memory are not bounded. Untested.
- Code-size boundedness is measured on ctsem and the canary only. A row-varying data-controlled shape, such as an inner trip count read from data per row, should grow programs= and must still beat the old stream on memory. Untested.
- prefer_frames counts prologue versions in the eight-trip sample, which overestimates slightly. Harmless.

**Small confirmed cost inside seal:** sorting live ranges is 445 samples, about 40% of seal time (`structured_frames.inc:250-263` plus collect_live_ranges). Arena allocation is bump-ordered, so the input is nearly sorted. Cheap to fix, worth about 5% of recording.

## The draft flat recording program

I found no semantic divergence from run(). Continue inside a while condition subtree is patched to the test instruction, matching the tree evaluator. Zero-trip For jumps to exit. Break and Continue at region top level fall out through the sentinel program counters. Exceptions propagate identically and per-Execution ownership means no state survives a failed recording. Only the visits counter is dropped, which is diagnostics.

Its honest ceiling is 15 to 20% of recording time, because leaves still call run() and the leaf work dominates. It is a reasonable stepping stone only if you intend to specialize leaves next. Stop criterion: build it behind the flag, A/B first-gradient wall at 4000 rows, and drop it if the gain is under 10%.

## Comparison of the three mechanisms

**Direct frame-relative lowering.** Ceiling 14%. Proof obligations are the largest of the three because encoding would move from post-hoc to speculative. Not now.

**Flat control program (draft).** Ceiling 15 to 20%, small proof surface, isolated behind frame admission. Fine as an experiment, weak as a destination.

**Data-only subtree execution on a register substrate.** This is the only lever that attacks the 4 billion visits. Static analysis is the param_dep fixed point the repository already had in the removed memo design (`docs/superpowers/plans/2026-09-03-structured-loop-static-storage.md:287-312`): imports start from data_only, kernels propagate, aliases propagate, and control dependence taints writes under parameter-controlled branches. A maximal data-only subtree with no effectful op, no Target, no active kernel, and initially no InPlace compiles to a closed program over plain doubles and integer loops. Exceptions are preserved by calling the same index kernels in the same order with a lightweight context, and descriptor shape checks can be done once per site because lengths are static. Live-outs get constant versions on exit, so recording, cell proofs, guards, and frames are untouched. Ordinary-model isolation is structural: the analysis runs inside prepare for retained plans only, which already exist for one ordinary corpus model. Realistic ceiling is 40 to 55% of recording if data-only work is 80% of the walk and the substrate is 3x faster per call. That is a hypothesis until the counter split below measures it.

## Smallest next change, and what falsifies it

Before either compiler, add one counter split to the existing tape diagnostic (`structured_loop.cpp:3121-3136`): visits and kernel calls partitioned into all-inputs-const, transient with a parameter input, retained, and in-place. The profile cannot separate data-only transients from parameter compares. This split fixes the data-only ceiling to within a few percent and costs an afternoon. If the all-const share of kernel calls is under 60%, the register-substrate plan is not worth its proof surface and the flat program plus descriptor pre-validation is the right scope.

## Prioritized plan

1. Counter split, one recording at 33 and 400 rows. Decision point.
2. Tape relocation for executor clones: fix up frame bindings, import destinations, outputs, targets, and workspace pointers by range lookup. Fixture: clone after recording, evaluate at a new point and after a data change, compare bitwise with a fresh executor. This removes K minus one first gradients per run regardless of what else lands.
3. Presorted live ranges in seal_frame. Measure, expect about 5%.
4. If step 1 confirms data-only dominance: data-only subtree compilation, InPlace refused, straight-line and loop-with-data-control bodies only, ablation flag, bitwise gate against the tree walk. Adversarial fixtures: a data-only loop whose bound is a parameter (must not compile), an index fault inside a compiled region (same exception, same point), a data-only region whose live-out feeds a parameter branch.
5. Otherwise: build the draft flat program and split validate_index into per-site static checks, each behind its flag, each A/B measured, each dropped below 10%.
6. Add the two limit fixtures regardless: Sequence{small For, big For} region, and row-varying data-controlled shape.

## Follow-up: specialized data instructions

The same Fable 5.1 CLI session inspected the v5 prototype with Read and Bash
tools after receiving the call census and feasibility measurements. This
review preceded final integration and validation. Raw output is in
`.cache/ctsem-first/fable-data-review.jsonl`.

**Verdict:** I could not construct a counterexample to the v5 proof. It is a sound bounded step, with one coverage trap and three places where a fixture should pin the argument rather than the reasoning.

**Why the proof holds.** DataKernel is only ever applied where run_transient would have computed `folded == true` on every execution. The chain is inductive: every initial version is const except non-data-only imports (`structured_loop.cpp:3033`, `3048`), which seed `varying`; every writer of a slot (kernel out, alias dst, For iterator, in-place base) propagates const exactly when its inputs are const (`1769`, `1800`, `1835`, `1964`, `2019`), and the fixed point in `recording_data.inc:23-44` mirrors each of those writers. Static non-varying therefore implies runtime `is_const` for every binding the site can observe. The runtime's cell proof and memo hits only add constness, never remove it, so the static predicate is strictly conservative. Parameter control not tainting is the same rule the tree path already applies: `run()` folds a const-input kernel under a parameter branch and the guard was logged before its consumers (`2025-2026`). v5 changes nothing about that.

**Specific hazards checked, none confirmed.**

- Read-before-definition: publication happens on first execution only, so before it the reader sees the initial slot version, exactly as the tree path does. After it, the binding cannot change because prepare enforces unique outputs and `mutable_output` excludes alias dst, iterator, and in-place base.
- Frame compaction: the transient version is always live at seal because `live(s.node_version[site])` runs for every site and the generation-nulling at `structured_frames.inc:227` skips Transient. Bindings and node_version are remapped together (`343-345`) and the const flag rides along in `constants`. The published bit in `instruction.jump` therefore stays valid across seals.
- Guard failure and retry: RecordingProgram lives in the Execution, which is rebuilt per forward, so publication bits and the ctx out/scratch pointers are recomputed. Transient versions are recreated with const false at `3057` and republished on first execution.
- Aliasing of the workspace: transient classification already forbids out as alias src, target, output, in-place base, or active-reader input (`414-417`), so nothing can hold the workspace across an overwrite.
- Overrides and effects: a custom forward or effectful op taints out, so those sites never specialize. `s.effects` is write-only in production, so dropping visits and effects accounting is diagnostics only.
- Exceptions: the same kernel runs in the same order with the same validated context, so faults occur at the same point. An exception on the first execution leaves nothing published; the Execution dies with it.
- Zero-trip and untaken arms: an unexecuted DataKernel never publishes, matching the tree.

**The coverage trap.** `kernel->forward != n.forward` taints out. Every existing row-scan, nested-scan, and break-scan fixture installs counting forwards on exactly the index and compare sites that a DataKernel would take, so those fixtures can never exercise the new path. Any validation that reuses them proves nothing about DataKernel. Falsifying fixtures must leave forwards untouched and assert `data_published > 0` through the new diagnostic.

**Fixtures that would falsify, if anything can.**

1. Parameter-controlled literal choice feeding a data-only kernel: `if (theta > 0) x = a else x = b; y = g(x)` with a, b data, y transient. Flip theta across evaluations, forward-only then gradient. Bitwise against the tree walk and the unflagged flat program.
2. Read-before-definition across the frame boundary: a transient site first executed in iteration 9 (after the prefix seal), read in iterations 1 through 8 through its initial binding, then read after publication. Assert the value differs between the two regimes and matches the tree.
3. Data-only in-place base updated in place each row and read by a DataKernel in the same row, with a later parameter write to a different cell of the same container. This crosses the cell proof and the DataKernel.
4. Injected fault on the third execution of a DataKernel site, then retry with a smaller data count. Asserts the retry re-records cleanly and publication does not leak.
5. A model whose region is Sequence{small For, big For}. Unrelated to v5 but still untested, and v5 compiles the body of whichever loop find_outer picked.

**What v5 removes and what it does not.** Per specialized call it skips the const-flag write, node_version rebind, inputs_const scan, the cell-proof probe, and the log_call check. It keeps bind_inputs, the kernel, and validate_index, which the profile put at 13.8% on its own. The count share says how many calls are eligible, not how much of run_transient's 12.6% exclusive is the skipped part. The right measurement is the same two-probe A/B you ran for v3, with `data_published` reported, plus a check that retained allocations and the frame diagnostic line are unchanged.

**Isolation.** compile_data runs only inside a frame-admitted recording, so ordinary models and prep never reach it. The one shared-state edit is `s.ctx[site].out.data` and `scratch` set at compile time; replay paths rebind those per call, so it is harmless, but a comment at `recording_data.inc:56-58` saying so would save the next reader the trace.

## Final ordinary-model attribution review

After all fixed controls completed, an explicit `claude-fable-5-1` resume
received their results and used its own inspection tools. Raw output is
`.cache/ctsem-first/fable-final-attribution.jsonl`. It found no new ordinary
execution or registration cost that justified changing recorder ownership
or adding an architectural workaround before delivering the draft.

Its proposed explanation for the small, roughly additive cold-call deltas
is extra executable-page first-touch after relinking. This remains a
hypothesis. Its suggestion of a baseline plus unused-text control could
help test that explanation. The previous crossed-linker study did not prove
it, and seeing the same binaries return to timing parity demonstrates
measurement variability, not layout causation. Likewise, nondeterministic
allocator reservations in A/A controls do not establish the cause of a
systematic A/B difference.

At review time the ordinary-model gate remained open. The user subsequently
deferred this investigation so ctsem optimization can continue. Reproducing
a layout cost would explain it, rather than establish its absence;
production padding, linker-order tuning and speculative recorder rewrites
are not supported by the current evidence.
