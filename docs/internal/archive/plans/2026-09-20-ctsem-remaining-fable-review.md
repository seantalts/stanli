# Fable 5.1 audit of remaining architecture

Reviewed base: `680a4763b256764476bca72b7916ad312c910701 plus direct-forward checkpoint 22c2d86b`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

Audit complete. Here is the report.

## Ranking and decisions

Phases are kept separate: first gradient is about 15.2 s, warm gradient about 0.47 s, peak RSS 1.52 GB at 4,000 rows. Ceilings below come from the retained profiles, not from forecasts.

1. **Experiment zero, no production code: inline-aware first-recording profile plus per-seal counters.** The retained recorder profile attributes 44.5% exclusive to the recording dispatcher, which inlines the kernel, alias, materialize and index paths, and macOS `sample` cannot split them. This decides whether area 1 continues into binding work or sealing work. Keep, run first.
2. **Area 1a: recycle seal scratch and the per-frame Stream, fuse the version-table pushes.** First-gradient phase only. Semantics-free, bitwise gate plus identical frame diagnostics. Bounded ceiling about 8% of recording samples, realistic 3 to 5%. Keep as the smallest concrete change.
3. **Area 4: dedicated Fréchet backward for matrix exponentials.** Warm phase. Ceiling 3.4% of warm samples. Existing 60-digit oracle applies. Keep as a bounded later experiment after an extent census.
4. **Area 3 diagnostic only: classify every external frame reference by owner frame at seal time.** This is the generic independence test and it costs nothing numerically. Keep the diagnostic, defer any parallel executor. The reduce_sum vehicle is dead for this target for three verified reasons below.
5. **Area 2: relocating clone of a recorded loop state.** Startup phase only. Under 1% of a chain's gradient budget. Defer for the sampler. It is a real cost only for pooled multi-thread evaluation, where the prototype never records.
6. **Area 1b: proven-layout emission that skips the frame encoder.** Ceiling about 10% of recording samples, needs a new layout-equality proof. Defer behind experiment zero.

## Findings by area

**Area 1, measured facts.** The last first-recording sample is in `notes/performance/2026-09-20-ctsem-recording-sites.json:1456-1491`. It predates the solve work, but the recorder and sealer were untouched by post-392, so it describes the current recorder.

| Bucket | Samples of 7,698 |
| --- | ---: |
| recording dispatcher, exclusive | 3,426 |
| seal_frame inclusive / exclusive | 1,900 / 592 |
| encoder value + adjoint + sort + collect + hash map | 786 |
| version-table push_backs, three vectors | 311 |
| malloc + free + memmove | 287 |
| compare_forward, a trivial kernel | 609 |
| run_retained exclusive | 421 |

The frame diagnostic in `.cache/ctsem-post-392/baseline-profile/stderr:286` gives 3,995 frames, 4 programs, 638,725 code words, 2,655,409 external bindings and 384,514 peak versions. Per frame that is roughly 160k words, 665 external bindings and 19k copied cells. The peak version count is a maximum over seals and most likely describes the first seal, which covers the prefix plus eight trips at `runtime/src/structured_loop.cpp:2084-2089`. Do not derive per-row cost from it.

**Area 1, repeated per-seal work with no semantic content.** All in `runtime/src/structured_frames.inc`:
- Lines 209-211 allocate and zero two vectors sized to the whole version table on every seal.
- Lines 220-241 walk every slot and every site on every seal regardless of what the iteration did.
- Lines 250-263 rebuild a pointer-range list from the stream, filter it with a binary search per entry, then sort it.
- Line 361 replaces the Stream with a fresh object, so the roughly thirty pools in `runtime/src/structured_loop.cpp:1231-1275` regrow from zero capacity on every row.
- Lines 322-356 rebuild three parallel version vectors and two maps.

On the recorder side, `materialize` at `runtime/src/structured_loop.cpp:1669-1675` copies any constant workspace value into the arena on every consuming call, and folded retained calls still allocate arena and a version at lines 1796-1807 because data specialization requires transient storage at `runtime/src/structured_recording.inc:201`.

**Area 1a, smallest change.** Keep a persistent seal scratch on the loop state for the remap, lengths, live-id and range vectors, cleared but not freed. Keep one spare Stream and swap instead of allocating, with an explicit clear of every field. Replace the three version vectors with one record vector so `make_version` performs one push. No encoding, guard, lifetime or ordering changes.

Proof obligations:
- Every Stream field is reset. The import list must be empty for iteration frames, because lines 310-314 append it to the tape on every seal and a stale import would duplicate the runtime import checks at lines 559-566.
- The moved-from undo vector at line 284 must be a valid empty vector before reuse.
- The spare Stream must be released in `finish_frames` so steady-state retained allocation is unchanged.
- A failed guard or an exception mid-iteration must start the next recording from an empty stream, as line 3209 does today.

Gate: bitwise outputs and gradients at four sizes and three points, plus an identical frames, programs, code_words, cells and bindings diagnostic line, plus exact live-allocation parity after the first gradient. Fixtures: the existing forced-frame suite around `tests/test_structured_loop.cpp:4836-5829`, retry after a guard change, injected failure after automatic selection, zero-trip outer loop, aliases with in-place writes, value-only interleaving, and two copied executors recording concurrently. Cheapest discriminating experiment: the two-pair N400 probe used for the index work, then the fresh inline-aware profile.

**Area 1b, why deferred.** Programs equal 4 for 3,995 frames, so almost every seal re-derives a program it already has. Skipping the encoder needs a proof that the same instruction sequence, the same arena allocation sequence and the same external classification imply the same words. The fallback is safe because the stream is still complete, but the ceiling is the 786 encoder samples plus part of the 592 exclusive, and the external binding list must still be walked to build per-frame bindings. One anomaly to census first: 609 samples in a three-instruction comparison kernel means call volume through the data-kernel indirect path. Count executions per published data site per frame and check writer-based outer-loop invariance of their inputs before proposing any hoisting.

**Area 2, measured facts.**
- `runtime/src/executor.cpp:388-396` copies the graph and values, then rebinds, and rebinding constructs every kernel state fresh at lines 499-506. The loop state is built empty at `runtime/src/structured_loop.cpp:3026`. `runtime/include/stanli/kernel_types.hpp:19-21` has no clone hook. Nothing recorded crosses executors.
- Sampler order: `runtime/src/capi.cpp:633` clones before the chains run, and each chain's first gradient is inside its own initialization at `runtime/src/nuts.cpp:56`. `tools/stanli_run.cpp:181` is the same. No chain has a recorded source unless the caller evaluated the prototype earlier. That is an assumption I could not verify for API clients.
- Pool order: `runtime/src/executor_pool.cpp:102` clones the prototype on demand, and `runtime/src/bridgestan_abi.cpp:325` builds the pool from a prototype that is never leased. So the prototype never records, and every thread's first lease re-records. This is a concrete current cost.
- What is shareable: only the program words, which are immutable shared pointers and total about 5 MB. Frame values are written during every replay at `runtime/src/structured_frames.inc:414-447`, so they can never be shared. A recorded clone is therefore a deep copy with pointer relocation of about 614 MB of cells and 2.66M bindings.
- Safety at another point: replay re-checks data imports and every guard at lines 559-568, so a relocated clone replays or respecializes exactly like its source, and respecialization at `runtime/src/structured_loop.cpp:3186` touches only that executor.

Expected pointer closure is frame values plus the loop workspace. Imports are copied into the arena at line 3227 and relocated, transient outputs live in workspace at line 1764, and iterator cells are workspace or arena at lines 1963-1980. This is not proven. A clone must classify each pointer against a range table and return null on any miss, falling back to a fresh state.

Smallest mechanism: a virtual `clone` on the kernel state returning null by default, and a substitution step in the executor copy constructor after rebinding. Measurement: copy cost plus the copy's first gradient at 400 and 4,000 rows with and without the hook, using the clone benchmark in `tools/bench_executor_clone.cpp`, and the fixture at `tests/test_structured_loop.cpp:4868` extended to assert zero respecializations on the copy.

Decision: defer for sampling. At 4,000 rows the first gradient equals about 32 warm gradients, which is under 1% of any multi-thousand-gradient chain, and there is no peak-RSS win because recording peak and steady live storage differ by about 0.1 GB. Moving the recording boundary before cloning in the C API is a design change and needs a sampling measurement under the plan's rule. Keep it only if the user's workflow is pooled multi-thread evaluation.

**Area 3, verified blockers.**
- Both nesting orders are closed at lowering. `runtime/src/lower_higher_order.cpp:581-582` refuses a parallel reduce_sum inside any retained region, and line 623 disables retained lowering in every child. Even with that line flipped, import compaction at `runtime/include/stanli/detail/input_ranges.hpp:57-67` rejects any stateful kernel, and the loop kernel registers state at `runtime/src/structured_loop.cpp:3406`. It also rejects dynamic index ops, which the target body uses at hundreds of sites.
- The saved target MIR at `.cache/ctsem-post-392/inputs/ctsm-o0.mir` contains zero reduce_sum occurrences. The reduce_sum vehicle therefore yields nothing for this target without model changes.
- Inside the loop kernel, per-site contexts are shared by every frame at `runtime/src/structured_frames.inc:383`, and transient outputs point into one shared workspace. Parallel frame replay needs per-worker context arrays and per-worker workspace relocation. Body kernels are stateless by construction, since `runtime/src/structured_loop.cpp:157-169` refuses nested loops and stateful kernels, so a context copy would suffice for that part.
- In-place bases and copy sources can be external references into earlier frames, so a partition can race on earlier frames' values. Adjoint accumulation into import and prefix adjoints is a running sum across frames in reverse order, so no partition is bitwise for parameter gradients. The target reduction at lines 1336-1348 is position-based and stays bitwise if target slots are fixed.

Generic boundary, no name recognition: a maximal run of consecutive frames whose external value bindings, in-place bases, gather and copy sources, adjoint bindings and promotion links reference only the prefix frame or frames inside the same run. Runs are the schedulable units. Contract: parameter gradients equal the sum of per-run partial sums merged in run order, validated by the external tolerance gate, with a serial mode that stays bitwise. Exceptions rethrow the lowest frame index, as `runtime/src/reduce_sum.cpp:61-62` does by job index. Any guard failure respecializes the whole tape, as today.

Cheapest discriminating experiment: at seal time, classify each external reference by owner frame and emit run count and cross-run reference counts per frame under the existing diagnostics flag at 400 and 4,000 rows. If the run count is one, the state is carried across all rows, and unknown independence is a refusal. Defer implementation until that number exists.

**Area 4, measured facts.** In the warm sample under `.cache/ctsem-post-392/baseline-profile/warm-sample.txt`, the exponential backward has 257 samples and the dynamic forward 77, of 7,611. The backward at `runtime/kernels/matrix_fns.cpp:235-249` exponentiates a doubled block whose Padé order and squaring count come from the block norm, so its cost scales like eight forwards. A Padé-based Fréchet derivative of the transpose in the adjoint direction computes the same top-right block at size n for about three forwards. Ceiling is 3.4% of warm time if backward became free; a realistic gain is 2%. The observed extent is not in any census I found, so an extent census of the dynamic call comes first.

Test path is cheap and exists. `tools/matrix_pullback_hp_check.py:74-85` already holds the block reference at 60 digits, and `tests/test_native_matrix_pullbacks.cpp:249-277` holds a long-double finite-difference check. The nested-tape comparison at line 516 must become both-versus-oracle, since a changed algorithm will not match the block method to 1e-14 on every input. Adversarial set: non-normal and Jordan-like matrices, nilpotent, large norm with many squarings, extents 0, 1 and 2 where the forward uses a closed form, zero adjoint seed which takes the scale branch, nonfinite inputs, invalid dynamic extents. Decision: defer behind items 1 and 4, keep as a bounded experiment, and do not combine with Padé-intermediate retention.

## What I did not verify

- The composition of the recorder's 3,426 exclusive samples. Only an inline-aware profile answers it.
- Steady-state per-row version count and the true cost of the per-seal allocations.
- Whether any external frame binding targets a preceding iteration frame. This single number decides area 3.
- Whether API clients evaluate the prototype before sampling.
- The observed matrix-exponential extent.
