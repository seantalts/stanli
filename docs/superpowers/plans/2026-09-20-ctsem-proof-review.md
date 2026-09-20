# Fable 5.1 follow-up proof audit

Reviewed base: `680a4763b256764476bca72b7916ad312c910701 plus architecture checkpoint 1b2ad991`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

**Scope note first.** The audit covers the working tree, not just 1b2ad991. Another session is editing concurrently: `runtime/src/structured_frames.inc` now contains the Packet A matcher (`FrameEncoder<Matching>`, lines 37-271, wired at lines 364-402), `FrameTape` gained hit counters (`runtime/src/structured_loop.cpp:1367`), the kill switch is at line 1454, and `frame_layout_tests` exists at `tests/test_structured_loop.cpp:5863`. My first read of the file predated those edits. Findings below are against the current contents.

## Packet A: checked layout matcher

**Verdict: sound as execution equivalence, not as representation identity.** No direct counterexample found. One implicit invariant should be made explicit, and one test bound should be tightened.

Equivalence argument. For every word position in the candidate, the interpreter's resolution (value pointer, adjoint address, literal, position slice) in the matcher's frame equals what it would be in the frame the canonical encoder builds from the same Stream:

- Scalar words (site, flags, arity, counts, decision, old offset, copy length) are compared exactly in `word()` at `structured_frames.inc:116-123`. Header word counts are checked at line 257.
- A currently local operand returns its snapshot offset at lines 91-94 before reaching `bind()`, so the one genuinely dangerous case (canonical external, now local, which would bind a pointer into an arena that `s.arena.clear()` reuses at line 449) is rejected by the word compare. Null and zero-length operands go the same way.
- A currently external operand at a canonical external word is bound or verified at exactly that index (lines 61-76). The dual of aliasing, one canonical index now holding two distinct pointers, is rejected at line 71.
- Adjoint identities compare full (id, shift, promotion) at line 71 via `FrameAdjoint::operator==`. Local adjoint words fold shift identically in both modes (line 105).
- Position slices are compared elementwise over the referenced range (lines 128-137). Literals are pushed in the same order with the index verified by the following word compare.
- Kernels never run during matching. Rollback is complete: `encode(const Stream&)` mutates only the three vectors cleared at lines 376-378, and the encoder starts from those same empty vectors. `frame->old`, `values`, `adjoint_base` and the tape's promotion links are set before either path runs.

Answer to the aliasing question. A canonical code may bind two formerly distinct external indices to one now-identical pointer or adjoint identity. The only consumers of a binding index are `FrameExecution::value` and `adjoint` at lines 458-469 (pure table lookup) and diagnostic counts. Aliasing acceptance never introduces an alias: the kernel already ran during recording with those exact pointers, and the encoder's own frame would present the same pointer pair under one name. The `frame_layout_tests` mode 0 fixture (theta,theta) after (theta,beta) is precisely this case. Consequence to state in the plan: the certificate is strictly weaker than "encoder output equals candidate", so hit counts and `tape.codes` population are not comparable to the encoder's, and tests must compare execution, never code identity.

Gap between plan text and code. Plan item 6 promises matching of "active/backward instruction structure". The matcher never reads `expected->backward`. Soundness currently rests on an implicit invariant: backward membership is determined by words. That holds for Call (flags word), InPlace and Copy (always backward), and Gather only because an active gather is always Retained (`structured_loop.cpp:426`), so its output gets a fresh `reserve_adjoint` at or above `tape.adjoint_base` and its destination adjoint word is never `frame_null`. The existing hash dedupe at lines 392-396 relies on the same invariant, so this is not a new hazard. Recommend a lockstep cursor over `expected->backward` checked at each instruction end and at completion. Cost is negligible.

Test findings and the cheapest required near miss:

- `tests/test_structured_loop.cpp:5943` asserts `matches >= 3 && matches < 5`. That admits both accept-aliasing (4) and reject-aliasing (3). Pin the exact value so the behavior you asked about is actually tested. Add exact counts for modes 1 and 2, which exercise position-slice refusal.
- Because `frame_layout` defaults on, every existing frame fixture (promotion at 4957, branch changes at 4874, while exits at 4903, injected failure at 5042) already runs through the matcher and compares bitwise against baseline and tree. Cite that as differential coverage.
- The class black-box fixtures cannot reach is a local-to-external transition with identical instruction structure, since folding and ownership changes co-occur with structural changes. The cheapest required check is therefore white-box: an env-gated debug mode in `seal_frame` that, on a hit, also runs `FrameEncoder<false>` into a scratch frame and asserts per-word resolution equality plus equal backward sets. It is recording-time, not a warm-replay check, so it does not violate the plan's prohibition. Run it under the frame suite and all four ctsem sizes.

Benefit bound. The first-recording profile attributes the following to sealing:

| Samples | Total | Sealing share |
| ---: | ---: | ---: |
| 582 | 7,661 | 7.6% |

The matcher still pays the snapshot copy, the `local()` binary searches and the binding pushes, so the achievable first-gradient reduction is a few percent at most. The plan's "below parity" criterion is the right reading.

## Packet B: dependency diagnostic

The saved probe (`notes/performance/2026-09-20-ctsem-frame-dependency-probe.patch`) walks `frame.bindings` without knowing operand roles, so it cannot separate reads from writes or carries from resets. Concrete replacement:

1. Walk each frame's `code.words` exactly as `FrameExecution::forward` does but without executing. Emit one record per reference: frame, kind, role (call input, out, out2, scratch, InPlace base/rhs/selector, Copy src/dst, Gather src/dst, guard, Set), mode (read, write, read-modify-write), extent (from the site's slot lengths, `pos_count`, copy length, or the position set for InPlace), and storage class (own values, frame j values, workspace, unknown).
2. Sweep frames in order maintaining last-writer per cell. A read in frame i of a cell last written by frame j < i is a true RAW carry unless frame i wrote that cell earlier in its own program. Report WAW and WAR separately with true distance. Group by program identity (`code.get()`) so boundary-program frames are visible.
3. Sweep the `backward` lists in reverse frame order over adjoint references: accumulation (`+=`), read, zeroing (InPlace `a[pos]=0`), and old-value restores. Count adjoint cells accumulated by more than one frame.
4. Report workspace cells written by more than one frame, and note `s.ctx[site]` (line 473) as a structural hazard shared by every frame that calls the site.

Two conclusions that follow from the interpreter as written, not from the census:

- An InPlace reset cannot kill its own dependence. Forward saves `old[k] = base[pos[k]]` at line 513 before writing, and backward restores it at line 565. Only whole-container rebinding (Alias, Copy destination, fresh Retained output) removes a carry. The plan's "overwrite-before-read reset may remove only the dependence it kills" is correct, and for indexed writes it removes nothing.
- Every frame accumulates into shared adjoint cells (`adj_prefix`, `adj_import`) with `+=`. Under the bitwise gate, any partition of the backward pass changes association order. Backward parallelism is excluded regardless of what the forward census says, unless per-cell accumulation is provably single-frame. The diagnostic in step 3 answers that directly.

I do not propose parallel execution. The stopping artifact is the role-aware witness table.

## Packet C: recorded-clone relocation

The pointer closure is enumerable, and the proof is easier than B's because it needs classification only, no ordering:

- Per frame: `bindings` (must resolve to frame j values or `s.workspace`, else refuse), `adj_bindings` (indices, copy verbatim), `values`/`old`/`literals` (deep copy), `code` (share).
- Per tape: `imports[].dst`, `outputs`, `targets` (may be workspace pointers via the `relocate` passthrough at line 405), `output_adjoints`/`target_adjoints`/`promotions`/`promotion_links` (copy), `adjoints`/`target_work` (fresh), `codes` (share).
- Per state: fresh `ctx` from the plan, copied `workspace` contents, `reverse_ready` false.

Transactional refusal: build the clone tape in a temporary and swap only if every pointer classifies, `tape.ready` is true, and `s.building` is null. Stream mode is a separate relocation problem and should refuse. The probe's `value_unknown` and `adj_unknown` counts are the necessary observation for this proof. `Executor::Executor(const Executor&)` at `runtime/src/executor.cpp:388-396` calls `bind_()` and creates fresh kernel state, so there is no clone hook today; this needs a new kernel API. Per the architecture notes, the sampler clones before recording, so the proof alone yields no sampling benefit without moving the recording boundary.
