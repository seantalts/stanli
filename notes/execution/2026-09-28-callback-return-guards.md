# Callback return safety before broader exit lowering

Continuation of the native-priority interpreter-gap work. This is a
correctness guard within the existing register compiler, not native codegen
or a claim of faster execution.

## Reproduced problem

`ProgramCompiler::Returned` unwinds the C++ compiler to end function lowering.
A plain return inside a runtime `while` used that exception even though the
loop's exit jump had not yet been patched. The baseline reported `prog.ok`
for `f_while_early`, where the loop must choose between an early and trailing
return; `.cache/callback-return-baseline.log` records the unexpected admission.
The malformed program was not executed: its unfinished jump could loop.

The paired-terminal-return optimization had the analogous problem inside
an enclosing runtime branch or loop: its local join does not also represent
the enclosing function exit. Its C++ exception could skip the outer arm and
trailing statements. A nested return pair therefore needs the same guard.

## Change and proof boundary

- Refuse function returns beneath a runtime branch or a runtime loop entered
  within that function. Solver callers retain their existing MIR fallback.
  Other callers retain their existing refusal policy; this does not add a
  whole-model interpreter fallback.
- Admit the existing paired-terminal-return optimization only at the current
  function's top runtime-control level.
- Track the loop depth at a callee's entry, so an ordinary callee return inside
  its caller's loop remains legal. Restore the prior depth when returning.
- Restore the caller's loop-frame count after an inlined call. A compile-time
  return from an unrolled callee `for` must not leave its frame in the caller,
  where a subsequent break/continue would otherwise bind to the wrong loop.

These checks use lexical/runtime control facts, not function names. No
runtime instruction, extra per-evaluation test, or numeric derivative rule
is introduced. Previously admitted incomplete programs become refusals;
that is an intentional correctness change, not a coverage improvement.

## Evaluator

`odefns.stan` / `test_ode_prog.cpp` add a direct runtime-loop return, a nested
return pair inside a branch, and a pair inside a loop. All refuse with a
reason. Their interpreter callbacks are checked against the function's
explicit formula on both sides of the time and sign guards. A positive
canary calls functions with both ordinary conditional returns and returns
from an unrolled `for` inside a caller's runtime loop containing `continue`;
its register values match the interpreter across twelve changing inputs.
The existing straight-line generated-derivative and callback tests remain.

Final validation: **273/273 native CTest tests passed** and **329/329 recorded
CmdStan models passed** at three points with 1,020,194 values. Existing
model-specific ULP and scaled/structural gates were unchanged. No numerical
performance gain is attributed to these safety guards. Logs:
`.cache/vector-rng-callback-final-{build,ctest,corpus}.log` and
`.cache/callback-return-{baseline,tests}.log`. Formatting passed.

## Next design boundary

General return support needs a common function-exit representation: explicit
jumps to a function epilogue, a result with validated logical shape at every
exit, and correct branch/loop history for reverse execution. Extending another
syntax-specific return-pair exception would repeat this bug. That work must
cover early exits, nested calls, zero-trip loops, rejection/print/RNG effects,
shape-mismatched returns, and ordinary direct-RK canaries. Variable-size return
storage and recursive call frames remain separate larger obligations.

The current fixed-shape region/kernel engines remain the target. Native
instruction generation, stencil JIT and dispatch-JIT research stay tabled.
