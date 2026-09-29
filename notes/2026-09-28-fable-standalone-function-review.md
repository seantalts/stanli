# Fable review: standalone function plan

Read-only `claude --model fable --effort medium` review, before implementation.

Review of `notes/2026-09-28-standalone-function-plan.md`, based on function.cpp, the ProgramCompiler region in mir_prog.hpp, the function-exits note, and test_function.cpp. Findings are ranked by how much they change the plan.

## P1: the plan is wrong or silent on these

**1. The compiler's evaluation hooks must be explicitly absent.** The compiler exposes `extern_real`, `extern_ints`, and `extern_int` (mir_prog.hpp:139-160). Each one runs a data-only user function through the host interpreter during compilation. Installing any of them for standalone functions folds runtime real argument values into a program, and it executes user code before the compile has succeeded. A print or reject inside such a function would fire during compilation, and then fire again in the interpreter fallback if the compile later bails. That breaks the plan's own rule that only a pre-execution refusal may select the fallback. The plan should state that these three hooks, plus `lower_higher_order` and `bind_target`, are left empty so every refusal is a pure compile-time bail with no side effects.

**2. "Integer values specialized by compilation" means every integer argument, and the plan does not say so.** This compiler treats scalar integers as compile-time constants (`ints`, `int_decl_at`, mir_prog.hpp:101-107). Caller-seeded names are treated as declared outside everything, so integer arguments land straight in the fold table. Only a structured while that writes an integer moves it to a register (`reify_written_int`, line 193-208). Two consequences follow:

- The cache key must contain the full value of every integer argument, arrays included, unless the compiler is instrumented to report which folded entries were actually read. No such read-set exists in what I read. An `array[] int` argument of length 1000 costs a 1000-int hash per call.
- Counted loops are unrolled by extent, so each distinct vector length compiles a fresh program of size proportional to the extent. A helper called across many observation lengths thrashes a bounded cache and pays compile cost every time.

The plan's measurement list only covers repeated identical calls. Add a varying-extent and varying-integer call sequence compared against the interpreter baseline. That is the loser shape. Also, promotion must happen before seeding: an integer passed to a real formal must be bound as a real register, not folded, or `real_identity(41)` keys on the value 41.

**3. Admission must key on depth and leaf, not on rank.** `array[] vector` has rank 2 like `matrix`, but DataMap stores the whole shape first-index-fast (mir_interp.hpp:830 and 1583, data.hpp:45) while registers are outer-major over array extents with column-major leaves (mir_prog.hpp:55-57). Admit depth zero, or depth one with a scalar leaf. The evaluator needs both sides: `matrix` admitted, `array[] vector` and `array[] matrix` refused with fallback.

## P2: missing evaluator cases

**4. `_rng` user functions.** Register compilation refuses RNG draws outside generated quantities (`in_write_array`, mir_prog.hpp:112-114). function.cpp:237 constructs the interpreter with no seed, and the C ABI has no seed parameter. I could not verify what the interpreter does today for a standalone `_rng` call. Both paths must agree, either both refuse or both draw from one documented default. Adding a seed is an ABI decision and should be called out as one.

**5. Integer semantics in double registers.** Runtime integers live in double registers (mir_prog.hpp:118-120). Test the 32-bit overflow boundary, integer division truncation, modulus with negative operands, and integer results returning both the `i` and `r` mirrors. test_function.cpp:176-178 already asserts both mirrors for the interpreter path.

**6. Return metadata.** The function-exits note requires every return arm to share one logical view including dimensions. A return whose extent depends on a real-valued branch must refuse rather than truncate. Void functions are unspecified: the writer is always invoked today, so both paths must agree on what a void call writes. Missing typed return metadata should refuse, as planned, but that refusal needs a fixture.

**7. Error text parity and the recursion guard.** Existing tests match substrings such as "recursion too deep" and "rank 0, expected 1". Reject messages, out-of-range index messages, and Stan Math domain-check text must be identical across paths, since rejection fidelity is a stated project priority. Recursion refuses today so the guard survives, but add a boundary test at depth 69 versus 70 so a future inline-recursion admission cannot silently delete the guard.

## P3: mechanics the plan names but does not specify

**8. Concurrency.** `stanli_function_call` takes a const handle (function.cpp:207), so the cache must be a mutable member behind a mutex. Copy the shared program pointer under the lock before executing, so eviction cannot free a running program. Do not evict on execution error. The refusal memo must use the identical full key. Kernel scratch is register-allocated (mir_prog.hpp:2055-2061), so per-call register files cover it. Whether the execution trace scope is thread-local is unverified. Confirm before asserting "zero interpreter entries" from concurrent tests, and assert it on the second call only, since first-call compilation may legitimately touch the interpreter.

**9. Zero extents.** Key on full dims, not element count. `matrix[0,3]`, `matrix[3,0]`, and `vector[0]` are distinct programs with zero elements. `checked_elements` accepts zeros, and the late-bound NaN fill (mir_prog.hpp:176-181) exists for zero-length adoption, so this path is real and needs a test.

**10. Memory.** Per-call register buffers scale with program register count, which scales with extent after unrolling. A cache bounded by entry count lets a few large-extent programs dominate memory. Measure the per-call buffer size and consider a total-register bound alongside the entry bound. The existing `kMaxRegs` refusal is the only backstop.

**11. Refusal coverage to enumerate.** Higher-order calls such as ODE solvers, algebraic solvers, map_rect and reduce_sum; complex leaves in bodies or results; and `_lp` bodies or `target()` reads with `target_reg` at -1. All should refuse and fall back. List them so admission failures cannot be masked by silent interpretation.

## Verdict

The alternative choice and the fallback discipline are sound. The plan is under-specified on three points that change the design: which compiler hooks are installed, what actually enters the cache key given integer folding, and shape admission by depth and leaf. Items 4 and the trace-scope point in 8 are unverified from my reads. The interpreter's `_rng` behavior is in mir_interp.hpp near the `BuiltinShapePolicy::Rng` check at line 2746, and the trace scope is in message_sink.hpp.
