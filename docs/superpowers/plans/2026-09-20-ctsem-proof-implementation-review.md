# Fable 5.1 follow-up proof audit

Reviewed base: `680a4763b256764476bca72b7916ad312c910701 plus architecture checkpoint 1b2ad991`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

**Verdict: accept A; accept C with test additions.** No code-level counterexample found in the snapshot for either packet. Items marked "fix" below are test or claim fixes, not implementation changes.

## Packet A against my earlier requests

- **Backward lockstep cursor** is present at `snapshot/structured_frames.inc:259-266` with the completion check at lines 274-275. The else-branch rejection is sufficient: consumed entries equal earlier instruction starts, header word counts force instruction boundaries to align, so the next unconsumed entry can only match the current start or a later one.
- **Exact fixture counts** are pinned for all three modes at `snapshot/test_structured_loop.cpp:5946`, with a diagnostic print on mismatch.
- **Resolution oracle** at `structured_frames.inc:286-385` compares resolved pointers, normalized adjoint identities, literal bytes and position slices, and runs only at recording time on hits. This is the white-box check I asked for, and it correctly tolerates alias-merged binding indices.
- Rollback on a failed match still touches only the three vectors cleared at lines 490-492. Nothing new to flag.

## Packet C proof audit

**Pointer inventory closes.** Every mutable pointer field is handled: frame `bindings` relocated (line 772-773), `imports[].dst`, `outputs`, `targets` relocated with full extents (774-780), `adj_bindings`, `promotions`, `promotion_links`, `output_adjoints`, `target_adjoints` copied as indices (783-788), `adjoints` and `target_work` fresh (789-790), `code` and `codes` shared as immutable. Site contexts are fresh because the plan rejects stateful body kernels at `snapshot/structured_loop.cpp:168-170`, and every context pointer is rebound per call at `structured_frames.inc:599-604`. Binding targets can only be arena cells, which live ranges snapshot including kernel scratch and imports (`structured_loop.cpp:2533, 2562`), workspace (`:1865`), or earlier frames' values. All three are in the range table.

**Length-1 remapping is sufficient.** The relocate at lines 754-771 is an ownership test, not an extent test. The offset within an identically sized copy is preserved, so every access valid in the source is valid in the clone. Extent safety is inherited from source validity. Imports and outputs check the full extent, which is stricter than the seal-time passthrough at line 531, so a dangling passthrough in the source refuses rather than propagates.

**Not inheriting memo, versions and adjoint_size is safe.** Guard fallback in the clone resets frames and releases state (`structured_loop.cpp:3269, 3287`), which is a fresh first recording. Replay backward sizes adjoints from the source tape at `structured_frames.inc:789`, never from the state field. The memo flag only gates diagnostics.

**Transactional under exceptions.** Every allocation that can throw precedes the two noexcept commits at lines 792-793, so a bad_alloc leaves the destination fresh.

**Isolation invariant to state in the plan:** a clone equals the source continuing from its current state, including a forward-only source with post-InPlace values and captured olds. That is why the odd-mode fixtures are sound and why the claim is correctly scoped to already recorded executors.

## Fixes and deferrals

- **Fix the concurrency claim.** At `snapshot/test_structured_loop.cpp:748-769`, the clone was last recorded at theta −.5 with selector 1, then the threads evaluate at (.75, 1) and (−.75, 2). The selector change and the sign flip of the cell read for the guard will re-record at least one clone in every mode. The test shows isolation, not concurrent replay over shared code. Evaluate both at guard-preserving points and assert no "frames:" diagnostic on either thread.
- **Add three refusal tests.** Stream-recorded source (frames null, `structured_frames.inc:722`). Backward exception (`structured_loop.cpp:3470` leaves the flag false). A clone constructed without the frames env var, exercising the frame_auto acceptance at line 720.
- **Defer, with disclosure:** the unknown-pointer branch at line 769 is unreachable from any fixture. Either add a white-box hook or document it as defensive and untested.
- **ABI note:** the new virtual at `snapshot/kernel_types.hpp:24` changes the vtable. In-tree `ReductionState` and `ReplayState` rebuild automatically, but any prebuilt kernel pack deriving from `KernelState` would be undefined behavior. Verify none exist.
- **Silent semantic change:** every copy of a warm frame executor is now a warm clone, including the existing tests at `test_structured_loop.cpp:4868` and `:5960`. Consumers who copy to obtain a cold executor must set the kill switch. One sentence in the plan.

## Reconciliation after review

No production implementation changes were required. The following test additions
pass with the canonical layout verifier enabled:

- Concurrent copies now use different guard-preserving negative parameter values
  with identical selectors, and assert that neither thread records new frames.
- A stream-recorded source refuses; the destination can subsequently record frames
  and an automatically admitted copy inherits them without a frames override.
- An injected backward exception after partial reverse work leaves cloning
  ineligible; the copied executor recovers through fresh recording.
- An extension-kernel fixture publishes a foreign scratch pointer. The completed
  source tape refuses relocation transactionally and still evaluates correctly.
- Destination replay refusal and non-target output relocation have direct coverage.

The plan states the changed warm-copy behavior and the disable switch. A complete
runtime/consumer rebuild covered all three in-tree KernelState implementations;
there are no prebuilt derived kernel packs in this build. External derived packs
would require rebuilding. Final CTest remains 262/264 with only the established
two signature-generation failures. The implementation library is byte-identical
to the selected performance archive.
