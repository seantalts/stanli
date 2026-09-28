# Fable review: shared function exits

Requested read-only CLI review using `claude --model fable --effort medium`.
The review saw the initial diff on 21937576 before the pending-loop fix.

## Dispositions

- Pending break/continue defect: fixed by recording the callee loop base and checking local pending jumps before selecting the straight-line return path. The differential loop fixture independently exposed this as a hang; it now passes. The independent ODE reference also exercises continue, break and return across its three points.
- Removed the obsolete paired-terminal-return special case from ProgramCompiler. Graph lowering has a separate implementation and is unaffected.
- Added the final-jump-elision invariant as a comment.
- Void statement calls remain outside this slice.
- Direct RK eligibility still excludes branches; general generated reverse is tested separately.
- Straight-line callback retains its six-instruction/no-copy canary.

## Review text

Review complete. Verdict and findings below.

## Verdict

The bounded design is sound and the implementation mostly matches it. The per-function `ReturnFrame` with explicit jumps to a shared epilogue, the distinction between a terminated lexical path (`PathExit`) and function completion, per-callee frames, and shape agreement via `same_view` are all correctly wired. The zero-overhead straight-line path is preserved. One concrete correctness defect blocks landing. One weakness in the plan's own claims should be closed by the tests you are still writing.

Base for this review: uncommitted diff on top of 21937576, files `runtime/include/stanli/mir_prog.hpp`, `runtime/src/ode_prog.cpp`, and the note.

## Concrete defects

**1. Straight-line `Returned` unwinds through an unrolled `for` with pending break or continue jumps, leaving those jumps targeting instruction 0.** At `mir_prog.hpp:3984` a return is treated as straight-line whenever `branch_depth == 0`, no structured while is open, and no prior result exists. But a trip of an unrolled `for` can already have emitted runtime jumps into `loops.back().breaks` or `continues` from an earlier `if (...) break;` in the same trip, or breaks accumulated across earlier trips. The `for` handler at `mir_prog.hpp:4017` catches only `CompileContinue`, `CompileBreak`, and `PathExit`, so `Returned` skips the patch loops at lines 4032 and 4039.

```stan
real f(real x) { for (i in 1:2) { if (x > 0) break; return x; } return -x; }
```

CmdStan returns `-x` when `x > 0`. The compiled program leaves the break's `JMP` with `dst = 0` and never compiles `return -x`. The `if (c) continue; return x;` shape fails the same way. The frame path already handles this correctly through `path_returned`, so the fix is narrow. Record `loops.size()` at `function_body` entry in the frame and set `runtime_exit` when any loop from that base upward has non-empty `breaks` or `continues`. A return inside a loop with no pending jumps stays on the zero-copy route.

**2. Nothing else rises to a defect.** Specifically checked and confirmed correct:
- Single unconditional return still throws `Returned` with no result allocation, copy, or jump. The `has_result` gate at line 3986 only diverts once a runtime exit has already been emitted.
- Per-callee scope. `function_body` swaps `return_frame` under RAII, and `inline_call` resets `branch_depth` and `return_while_base`, so a callee's return always targets its own epilogue even when the call site sits under a caller branch.
- Zero-trip and may-not-execute loops. `for` with `lo > hi` compiles nothing and does not propagate. `while` swallows `PathExit` and only drops the back-edge, which is right because the body may never run.
- Runtime branches. Both `if` arms compile regardless of the other arm exiting, the then-arm's join `JMP` is elided when it exits, and `PathExit` propagates only when both arms exit.
- Unrolled `for` with `continue` reaching a later trip. `path_returned` with pending continues patches them to the next trip and keeps unrolling. Pending breaks correctly suppress propagation because the break path reaches the statements after the loop.
- Effects. The return expression is evaluated inside the branch, before its `JMP`, so numerical work, RNG, print, and reject are path-gated.
- Shape agreement. `same_view` compares kind, extents, dims, and leaf but not `reg`, so replacing the register in the frame is safe. A mismatch bails rather than producing a wrong-shaped result.
- ODE callers. Both `compile_rhs_args` and `compile_dae_args` now use `function_body` and still enforce `out.len == n_y` on the frame result.

## Future work and unverified items

- **Void functions.** `same_view` on two `Range{0,0}` values returns false because the Flat rule requires `len == 1`, so a void body with two runtime `return;` statements would bail with a misleading message. Not reachable today: statement-position user function calls bail at `mir_prog.hpp:4175` and the only `inline_call` sites are expression contexts. Worth a `len == 0` short-circuit before void inlining lands.
- **Final-jump elision at line 3479** is correct only because every handler that patches a target to `code.size()` happens not to propagate `PathExit` from the same call. A comment or assert would guard a future handler from breaking it silently.
- **`peel_terminal_return` at line 4084** is now dead for every body compiled through `function_body`, which is all three callers. The general path emits the same `JZ`, `MOV`, `JMP`, `MOV` sequence, so bytecode should be unchanged, but the straight-line canary should diff bytecode rather than assume it.
- **Scope leak on `Returned` through `for`.** The loop variable in `ints`, its `int_decl_at` entry, and the `loops` element survive the unwind. `inline_call` restores all three and the ODE callers only run `finish` afterward, so this is hygiene rather than correctness.
- **Could not verify:** whether `compact_program` is jump-aware, and whether `JZ` and `JMP` are in the `exact_ode_adjoint_opcode` whitelist past `ode_prog.cpp:79`. Both were already exercised by `while` and the peel path in RHS programs, so jump-awareness is likely. If the whitelist excludes them, RHS functions with runtime returns fall off the direct RK path. That is a performance cliff to measure in the ODE phase timings, not a correctness issue.

The plan's test list already names return/continue/break combinations and zero-trip loops. Add the two shapes from defect 1 as compile-and-compare fixtures against CmdStan, since they compile today and produce wrong results rather than refusing.
