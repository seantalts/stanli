# Fable review: standalone compiled calls

Read-only `claude --model fable --effort medium` review of the initial implementation.

## Dispositions

- Non-Bail compile errors: confirmed with a negative-sized linspaced_vector beneath an untaken runtime guard. The regression failed before the fix. Catch ordinary compile exceptions as a pre-execution refusal; retain bad_alloc as a resource error. Never catch execution errors for fallback. Check the same issue in the shared callback entry points.
- RNG/null EvalState: ProgramCompiler.in_write_array remains false; rng_call refuses before emitting a CALL. The standalone unseeded-RNG test preserves the prior error.
- Input dimensions: validate_argument checks rank, checked dimension product and exact real/integer buffer lengths before cache lookup. Existing invalid-buffer/rank tests and new zero-extent tests exercise this.
- Replacement policy: measurements subsequently showed churn under simple FIFO. The updated policy requires the same missing signature twice after saturation; full-key equality still controls all cache hits, so admission-fingerprint collisions cannot select an incorrect program.

## Review text

Review complete. I used the full tool budget on the runtime diff, the MIR statement layout, the compiler argument/allocation API, and the compaction pass that renumbers seeded inputs. I did not get to the test diff or the plan note beyond the stat, so the findings below are from the runtime code only.

**Verified first: no execution-time fallback.** The compiled program only runs when the plan's ok flag is set, and that flag is set only after inlining, finish, and compaction all succeed (`runtime/src/function.cpp:249`). At the call site, run_function is invoked with no enclosing try, so any exception from run_program propagates to the outer handler and becomes a call error; the interpreter branch is reached only on a refused plan (`runtime/src/function.cpp:453`). Nothing is written to the result writer before execution finishes, so a failed run leaks no partial effects. This contract holds.

**Confirmed safe on the points I checked**

- Cache key and promotion. The call site clears the integer mirror for real formals before values reach the cache (`runtime/src/function.cpp:439`), so the key is dims-only for reals and dims plus contents for integers. No spurious churn from promoted data.
- Placeholder input pairs. The (-1, 0) entries for integer formals pass through compaction safely. Every seeded-span loop in compact_program_gated is zero-trip for length 0, and the in-file check and final remap are guarded on a positive length (`runtime/src/program.cpp:872`, `runtime/src/program.cpp:1070`). Zero-length real inputs at the end of the register file are covered by the same guard.
- Seeded reals survive compaction. They are pinned and marked used, and out_regs are remapped alongside them, so an identity return aliasing an input is still read correctly after the run.
- Concurrency. Compilation happens outside the lock, the double-checked lookup discards a losing racer's plan, plans are immutable shared_ptrs, and register buffers are per-call. Eviction accounting is consistent. In-flight callers keep an evicted plan alive by ownership.
- Refused compilations are cached per key. Compilation is deterministic for a fixed key, so the sticky refusal is correct, and no partial program from a Bail survives because the plan is reset in the handler.
- Integer argument marshalling matches the compiler's own convention: scalar ints carry no dims, depth-1 ints carry their DataMap dims, and the vector<int> to vector<long> assignment is a widening copy.

**Findings that need attention**

1. Non-Bail exceptions during compilation become hard call errors, not refusals (`runtime/src/function.cpp:265`). The catch handles only Bail. The compiler constant-folds integer-specialized arguments, so any std::exception raised while folding (a domain check, an index check that the compiler wraps only in some paths) escapes function_plan before caching, fails the call, and is re-attempted on every call with that key. The interpreter would have reported the same condition at runtime with its own message text, or possibly not hit it at all if the folded expression is on an untaken runtime path. This is both a fidelity question and a churn cost. Recommend catching std::exception in compile_function as a refusal, or proving the compiler never throws anything but Bail on the admitted subset.

2. Kernel calls run with a null EvalState (`runtime/src/function.cpp:355`). run_program's default state argument is nullptr, and the kernel path is taken whenever the program has calls. Recent commits compile RNG families into kernel calls. If a standalone function containing an `_rng` call is admitted, the compiled kernel will run with no RNG state. I could not verify whether the kernel dereferences the state or whether the interpreter path already rejects standalone RNG functions. If the interpreter rejects them, admission should refuse them too, since the compiled path would otherwise change behavior. Please check this one directly.

3. Cache-key correctness depends on validate_argument enforcing full dims (`runtime/src/function.cpp:215`, `runtime/src/function.cpp:229`). The key uses dims, but the compiled input length is fixed from the real storage size, and run_function copies that fixed length from the caller's storage. If validate_argument can ever accept a vector or matrix entry whose dims are missing or inconsistent with its storage size, two calls with different lengths would collide on one key and the copy would read past the caller's buffer. The matrix branch also indexes dims[0] and dims[1] without a size check. I did not read validate_argument; if it already asserts rank and product equality, this is not a defect.

**Minor, not defects**

- The storage estimate omits kernel udata owned by calls, which the comment acknowledges.
- The execution report is emitted for every call including cache hits, which is fine but means the diagnostic stream scales with call count.

No defects found in the locking, eviction, seeded-input remapping, or the run-after-compile-only contract. The three items above are the concrete places where the narrower candidate still depends on unverified guarantees from validate_argument, the kernel runtime, and the compiler's exception discipline.
