# Faster execution without surprising fallbacks

The goal is predictable native performance while preserving Stan's behavior.
Deleting `MirInterp` is a possible eventual result, not a reason to replace it
with something equally slow. We are extending the existing engines first.
Wasm is secondary. Native instruction generation and stencil JIT research
remain tabled. Fable review is reserved for rare, overarching plans.

This is the current plan after the September 28 coverage work. The detailed
[original inventory](2026-09-28-interpreter-gap-audit.md) is a historical audit;
its old refusal lists must be read alongside the completed changes below.
The task synchronized with `origin/main` at
`6ce2018b5382b459666ab32fe0df50d4a6d30ba4` before implementation.

## What runs a model today

A model can use all three engines in one evaluation. They call the same
precompiled numerical kernels and upstream Stan solvers where possible.

| Engine | What it does | Where it is useful |
| --- | --- | --- |
| Graph | Preparation assigns storage and binds each operation to a numerical routine. Evaluation walks those prepared calls. | Most log densities and generated quantities; vector and matrix work. |
| Register program | Preparation replaces named expressions with instructions over numbered storage cells. Evaluation runs arithmetic, branches, loops, and kernel calls. | Solver callbacks, standalone functions, and runtime control regions. |
| Structured loop | Preparation keeps a reusable loop body. Evaluation records executed operations and the values needed to differentiate them. | Eligible loops whose expansion would create a large graph. |

`MirInterp` instead walks the original compiler tree, looks up named values,
and constructs intermediate containers during evaluation. It still handles
preparation, constrained initialization, and unsupported callback/output cases.
Repeated use inside a solver can be expensive because the solver calls the
same function many times. A one-time preparation use may be insignificant.
Not every unsupported graph construct has an interpreter fallback: some are
compilation errors.

“Instruction dispatch” means reading an operation description and selecting
its implementation. The graph walks already-bound function pointers. The
register engine switches on instruction codes. Its registers are memory cells,
not processor registers. All three engines are compiled C++, but they do not
produce new machine code for each model.

## Derivatives are a separate choice

A fast value path can still have an expensive derivative path. Some kernels
have direct reverse routines. Others use Stan Math to record an autodiff tape:
values execute while recording how to pass derivatives backward. A kernel can
build such a tape even when the model itself uses the graph engine.

Register programs can also have a generated reverse program. This is another
prepared instruction stream, not generated machine code. Straight-line code
and eligible branches work; loop back edges currently require Stan Math replay.
The structured-loop engine already has its own history of iterations and
values. Reusing that mechanism is a possible way to improve callback loops.

Thus there are two different performance gaps to measure: repeated MIR tree
interpretation, and expensive differentiation inside an otherwise compiled
path. Removing one does not automatically remove the other.

## Gaps closed with the existing engines

| Previous gap | Current behavior and evidence |
| --- | --- |
| Early returns and eligible standalone functions | Shared register compilation handles fixed-shape exits through branches/loops; eligible standalone functions use cached plans. [Returns](2026-09-28-function-exits.md), [function API](2026-09-28-standalone-function-results.md). |
| RNG coverage | Shared graph/register handlers cover the migrated scalar and vector families, preserving draw order and validation. [Scalar](2026-09-28-scalar-rng-implementation.md), [vector](2026-09-28-vector-rng-implementation.md). |
| Runtime `for` bounds | Fixed-storage callbacks retain loops in the register engine. Lower bounds run once; upper bounds are reevaluated with Stan's semantics. [Coverage](2026-09-28-runtime-for-coverage.md), [semantic correction](2026-09-28-for-bound-semantics.md). |
| Runtime integer arithmetic/results | Typed integer operations and integer function results stay compiled. [Results](2026-09-28-native-integer-coverage.md). |
| Matrix and nested-array callback arguments | Complete dimensions and storage order survive both compiled and interpreted adapters. [Matrices](2026-09-28-callback-geometry-results.md), [nested arrays](2026-09-28-nested-callback-coverage.md). |
| Current-draw callback inputs | Generated quantities can pass runtime real and integer values into the existing solver kernels. [Reals](2026-09-28-callback-runtime-values-results.md), [integers](2026-09-28-runtime-integer-callbacks.md). |
| Current-draw solver controls | Generated quantities keep runtime tolerances, step limits, and adjoint-ODE controls compiled. Kernels call the same Stan Math overloads. [Results](2026-09-28-runtime-solver-controls.md). |
| Runtime matrix/nested-array indices and integer-array writes | Fixed-shape selections use existing gather/update kernels; loop writes invalidate stale integer facts. [Results](2026-09-28-runtime-index-coverage.md). |

These are bounded capabilities, not a claim that every overload, shape, or
control context is compiled. Each linked result records its independent Stan
checks, engine assertions, native measurements, and remaining refusals.
When Stanli and Stan disagree, Stan is the semantic reference. Bugs in
`MirInterp` must be fixed too; matching it alone is insufficient validation.

## Next decision: large constant loops

Expanding a loop copies its body once per iteration. This can make preparation
and storage grow badly and eventually exhaust the register limit. Simply
retaining every large loop is not yet a safe performance fix: a callback with
a back edge loses generated reverse support and the direct RK sensitivity path.

The [loop experiment](2026-09-28-constant-loop-decision.md) compares these
existing paths at 8, 128, and 2,048 iterations. Both remain register programs;
the experiment tests whether compact code also delivers faster execution.
No production unrolling threshold has been changed.

The [structured-callback experiment](2026-09-28-structured-callback-results.md)
now implements the first proposed adapter. It reuses structured history and
kernel backwards, and also tests grouping each iteration into an existing
register segment. Both are correct against the register oracle but slower in
complete ODE solves. At 2,048 trips, the simple case takes about 197 µs with
generated register reverse and 470–489 µs with the structured variants. The
production unrolling and callback policies remain unchanged.

The [loop-aware register reverse experiment](2026-09-28-loop-adjoint-results.md)
now tests that second option too. It versions overwritten values, preserves
copy aliases across iterations, and reuses existing derivative instructions.
Both a complete instruction history and a smaller block-visit history pass
bitwise checks. Neither beats the current callback: complete solves take about
1.5–2.3 times as long across the tested shapes and sizes. Recording the forward
history already costs more than the incumbent's complete callback gradient in
a representative local comparison.

The next decision is whether to invest in a more selective history layout:
keep ordinary forward registers and save only values required by reverse,
with explicit bindings for aliases and overwritten values. This is a separate
storage/compiler experiment, not permission to relax correctness or change
callback routing. There is no measured replacement ready to ship, and the
current fast expanded path should remain available.

The experiment also found a shared 24-ULP gradient discrepancy against CmdStan
in the large branch stress case. Both current and experimental callback paths
agree bitwise there. Its cause remains unresolved, and no tolerance was widened.

## What still needs a fallback

- Containers or slices whose sizes change during execution need a storage
  contract beyond the present fixed-size register buffers.
- A runtime `for` nested inside `while` still needs a correct lifetime for
  integer locals that reset on each outer iteration. Removing the guard alone
  would miscompile those resets.
- Standalone integer arguments remain specialized by value. Runtime integer
  callback inputs and controls described above are enabled for generated
  quantities; they do not establish general active-density support.
- DAE initial/output times still need preparation-time values.
- General recursion and runtime call frames need a broader function design.
- Preparation-time evaluation and constrained initialization still use
  `MirInterp`; prioritize them only if measurements show meaningful cost.
- Some numerical kernels still build local Stan Math tapes. Improve measured
  hotspots using Stan's algorithms, with numerical evidence for each change.

Deleting `MirInterp` would require replacement coverage for all its supported
roles, including errors, effects, recursion, dynamic storage, and preparation
probes. A build with production interpreter entry points disabled would need
to pass those contracts before deletion. Until then, a correct fallback is
valuable; the immediate objective is to keep ordinary models on fast paths.
