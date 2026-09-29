# Fable 5.1 implementation review of remaining architecture

Reviewed base: `680a4763b256764476bca72b7916ad312c910701 plus direct-forward checkpoint 22c2d86b`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

**Verdict: no correctness, lifetime, or regression bugs found in this diff.** Every scratch producer and consumer binds the QR buffer per call, not per site, and every read is guarded by the same eligibility the write uses. Change 2's clear list matches the `Stream` struct field for field, and Change 3 is a mechanical consolidation with a single construction site.

What I verified, by path:

- **Executor top level**: `runtime/src/executor.cpp:471-543` sizes scratch per op from the kernel at bind time, and copies recompute their own layout.
- **Var replay vari**: `runtime/src/executor.cpp:146-225` gives each call a private values arena that includes scratch, so a CALL executed repeatedly under var replay never shares a factor.
- **MIR interpreter**: `runtime/include/stanli/mir_interp.hpp:804-816` builds a fresh register file per kernel call.
- **Structured loop recording and historical backward**: `runtime/src/structured_loop.cpp:1851-1854` allocates out+scratch in one arena block per record, and `runtime/src/structured_loop.cpp:2261-2267` recovers scratch from that record's block. Stream replay uses per-call pointer words at lines 2888 and 3010. The reuse-primal path, which nulls scratch, is excluded for any kernel with nonzero scratch at `runtime/src/structured_loop.cpp:443-444`.
- **Frames**: scratch is encoded per call with its length at `runtime/src/structured_frames.inc:109` and rebound at line 388, so the factor lives in the sealed frame's values.
- **Island native adjoint**: `runtime/src/adjoint.cpp:59-61` rejects back edges, so a CALL inside a program loop never gets a generated backward that could read a shared scratch range.
- **Eligibility symmetry**: forward writes only when active, not value-only, and `a.size() != 0`. Backward reads only when active, `n > 0`, and scratch non-null (`runtime/kernels/matrix_solve.cpp:125,334`). A value-only forward leaves stale scratch, but every backward is preceded by its own active forward on every path above, and `test_solve_reuse.cpp` exercises that interleave in both loop modes.
- **Blocked QR**: `householderSequence(qr, coefficients)` constructs the same sequence type with the same length and shift as `householderQ()`, and the `conjugate()` of a real vector is the vector itself, so the n=55 bitwise check is expected to hold.

Material remaining validation gaps, ranked:

1. **prefer_frames boundary is unpinned.** `runtime/src/structured_loop.cpp:1537-1541` now estimates 24 bytes per version instead of 21, so loops whose estimate lands roughly in the top eighth below the 128 MB cap flip to frames automatically. Nothing in the diff tests near the threshold. Add a synthetic loop sized just under and just over the old boundary and assert the selected mode, or have the running four-size target report its mode per size.
2. **Top-level Executor solve is not covered by the new test.** `tests/test_solve_reuse.cpp` only places the solve inside `OP_LOOP`. Add one case with `OP_MDIVIDE_LEFT` at graph top level, interleaving `gradient` and `forward_value_only`, followed by an Executor copy, with the same bitwise assertions against a scratch-less reference.
3. **Var-replay and island paths have no solve-specific test.** Confirm an existing island or bridgestan var test routes a plain left solve through `executor.cpp:146-225` and `island.cpp:358-384`. If none does, add one. Note that each var-replay call now retains n·(n+1) doubles on the AD arena, which is a memory consideration for large n rather than a bug.

Hygiene notes for the change description:

- `runtime/kernels/matrix_solve.cpp:106` adds an `a.rows() == 0` early return to the TriLow left path that the removed code did not have. The result is the same empty matrix, but it is outside Change 1's stated scope and should be mentioned.
- "Releases scratch on failure" is deferred. `tape.sealing` is freed on finish at `runtime/src/structured_frames.inc:530`, but after a failed recording it only dies when the next forward resets the unready tape at `runtime/src/structured_loop.cpp:3229`.
- The four-size target and 329-model replay are still running, and the s2/hmm confirmation intervals include parity, so those remain open as stated.
