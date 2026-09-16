**Summary.** The working tree already contains the pooled register workspace (the island kernel file changed between my first and later reads, so HEAD line numbers are shifted by about 25). I reviewed the implementation that now exists. The design is sound and the invariant it relies on holds by construction, but one line blocks shipping, one gate is missing, and the expected gain is marginal against the target. The arena_allocator probe is superseded and would not help on its own, since value-initialization would still zero the buffer.

## Findings

- **High, blocks shipping. TLS object with a destructor.** `runtime/kernels/island.cpp:43` declares a `thread_local std::vector<var>`. The repo's written policy at `runtime/src/executor_pool.cpp:16-19` forbids anything with a destructor in TLS. Under MinGW emulated TLS such destructors ran after the DLL was unloaded and crashed R worker processes on Windows. Fix is a leaked heap pointer: `static thread_local Buffer* b = nullptr; if (!b) b = new Buffer;`. Cost is one buffer per thread that ever replayed, sized to the largest island seen.

- **Medium. The write-before-read invariant has no checker, and the failure mode gets quieter.** With zeroed handles a stale read is a deterministic null dereference. With cached handles it is a dangling `vi_` into recovered arena memory, which the arena keeps mapped, so ASan sees nothing and the gradient is silently wrong. The contract is stated at `runtime/include/stanli/program.hpp:431` and preserved by fill sinking via dominance (`runtime/src/program.cpp:567-573`) and by the compactor, but nothing verifies it. Recommend a compile-time must-write dataflow over the CFG reusing `each_read` and `each_write` (`program.cpp:39-101`) plus the predecessor scaffolding in `sink_program_fills` (`program.cpp:493-538`). Seed with the live-in ranges, intersect at joins, check every read span and every out register. Store the result on `IslandProg` and fall back to a fresh zeroed vector when it is false. One caveat: under var replay a CALL writes only its out range into the register file, because `run_call_var` (`runtime/src/executor.cpp:152-183`) uses its own arena buffer for scratch, while `each_write` claims scratch is written (`program.cpp:43`). Treat CALL as writing only out in the verifier.

- **Clean, no action.** Ownership transfer by swap gives a nested replay a distinct buffer and keeps the outer pointers stable (`island.cpp:50-55`). If `resize` throws after the swap, the cache is left empty and nothing dangles. The destructor swaps back on exception. `resize` value-initializes only the tail. `vin`, `reg`, and `vout` are disjoint sub-ranges (`island.cpp:133-136`). No accumulate-into-register pattern exists in any program source file. Instruction order and vari creation order are unchanged, so the 31-point bitwise gate should pass. Existing tests already cover nesting, a throw mid-replay, executor copy, and width change (`tests/test_island.cpp:2814-2881`).

- **On the raw-storage question.** Strictly in C++17 there is no way to create `var` objects without running a constructor, and the default constructor stores null, so any raw-arena scheme either zeroes or is formally undefined behaviour. Assignment into `alloc_array<var>` is well-defined only under implicit-lifetime rules, which are C++20 applied as a defect report. The pool of constructed objects is the simplest mechanism legitimate under C++17 wording, so it is the right pick.

## Performance expectation

From the profile, allocation, zeroing, and free inside the replay are the following share of samples:

| Region | Samples |
|---|---|
| bzero of register buffer | 289 |
| malloc and free of register buffer | ~155 |
| island backward total | 1354 |
| gradient, excluding clock instrumentation | ~1580 |

That is roughly 28% of the gradient. Applying it to the current ratio gives about .82 against a .8 target, which is inside seed-to-seed noise on four seeds. The next generic lever is `binary_bwd_typed` at `runtime/kernels/scalar_binary.cpp:42-77`. It builds a nested tape plus three heap vectors on every CALL backward. In this profile the lmultiply backward alone is 175 samples, about 13% of the island backward, and roughly 100 of those are malloc, free, and nested-tape setup. A closed-form backward for the scalar binaries with trivial partials is a kernel change, not model detection. One bitwise trap: for a broadcast scalar, `grad` accumulates lanes in reverse creation order, so a hand loop must accumulate descending.

## Tests to add

1. Verifier unit test with a hand-built program that reads a register before writing it on one branch, asserting the zeroed fallback is taken.
2. A debug mode that pre-fills the workspace with a var pointing at one heap sentinel vari and asserts its adjoint is still zero after `grad`, run over the corpus.
3. Extend `test_worker_lifetimes` (`tests/test_island.cpp:2777`) with islands wide enough to force per-thread buffer growth on each worker.

**Decision.** Proceed with the pool as implemented after replacing the TLS vector with a leaked pointer and gating the fast path on the verifier. Measure the full CLI. If the ratio lands below .8, take the scalar-binary backward next rather than changing architecture.