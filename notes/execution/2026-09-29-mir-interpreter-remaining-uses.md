# Remaining uses of the MIR interpreter

Source audit, September 29, 2026. Base: `df86223160f1387a37ed47c267df1bf31e77f11c`
(main after PRs #407 and #408). No runtime changes, new benchmarks, or model
coverage sweep were performed for this report. Examples below are supported by
current source and existing test assertions; those tests were inspected, not
rerun. Proposed improvements are opportunities to measure, not speedup claims.

The goal is to prevent surprising native performance cliffs. The remaining
interpreter uses are not equally important: repeating a callback thousands of
times can matter much more than evaluating a dimension during model loading.
There are still useful gaps to close with the existing engines before designing
anything to replace the interpreter wholesale.

## Where it still runs

MIR is the compiler's tree of statements and expressions. `MirInterp` walks that
tree, resolves named variables, and constructs intermediate values. The graph
engine instead executes prepared kernel calls; the register engine executes
prepared instructions over numbered storage locations. Structured loops retain
reusable bodies and record the work needed for differentiation. None of these
requires generating machine code for each model.

| Use | When it happens | Why it remains | Performance significance |
| --- | --- | --- | --- |
| Solver callbacks | Each callback evaluation when register compilation refuses the body | The solver still needs a callable function with the original behavior | Highest potential amplification: repeated within a solve, itself repeated during gradients or output generation |
| Transformed parameters and generated quantities (`write_array`) | Each requested output evaluation when graph lowering cannot handle the complete output program | There is no general continuation from a compiled prefix into an interpreted suffix | One small unsupported expression can move the entire output program onto MIR |
| Standalone function API | Each call for a refused or temporarily unadmitted specialization | Type/shape restrictions, bounded cache policy, or unsupported body | Important for repeated calls from Python, R, or C++; separate from model-internal function inlining |
| Data and transformed-data preparation | During model loading | Executes user transformed-data statements and establishes concrete values | Usually amortized, but large transformed-data computations can still dominate startup |
| Folding, shape, and admission probes | During lowering | Answers questions from the partial known-value environment | Can accumulate during preparation; not work repeated on every gradient |
| Constrained initialization / unconstraining | When a caller supplies constrained parameter values | Interprets initialization statements and evaluates parameter-dependent bounds | Usually infrequent; repeated unconstraining clients could make it important |

Ordinary log-density execution has **no catch-all whole-model MIR fallback**.
It executes the lowered graph, which may contain interpreted solver callbacks.
If an unsupported model statement cannot be lowered, compilation can fail.
Likewise, an interpreted output program can itself reject or encounter an
unsupported operation. “Fallback exists” does not mean complete Stan support.

### Solver callbacks

Five kernel families explicitly choose between a register program and MIR:

| Graph operation | Interpreted function | Source |
| --- | --- | --- |
| `OP_ODE` | ODE right-hand side, for legacy and modern interfaces | [ode.cpp](../../runtime/kernels/ode.cpp#L46) |
| `OP_ODE_ADJOINT` | Adjoint ODE right-hand side | [ode_adjoint.cpp](../../runtime/kernels/ode_adjoint.cpp#L20) |
| `OP_DAE` | DAE residual | [dae.cpp](../../runtime/kernels/dae.cpp#L20) |
| `OP_ALGEBRA_SOLVER` | Legacy or variadic algebraic system | [algebra.cpp](../../runtime/kernels/algebra.cpp#L27) |
| `OP_QUADRATURE` | Integrand | [quadrature.cpp](../../runtime/kernels/quadrature.cpp#L22) |

All use the same basic decision: `spec->prog.ok` selects register execution;
otherwise the adapter constructs a fresh `MirInterp<T>` and binds the callback
arguments. `T` can be a plain value or a Stan Math autodiff value. The surrounding
solver still uses its existing numerical kernel. A failed register compilation
is the fallback trigger; a runtime exception from an admitted program is not an
instruction to rerun the callback through MIR.

Important remaining refusal classes include:

- **Unproved runtime storage sizes.** For example, a callback declares
  `array[y[1] > 0 ? 1 : 2] real temporary`. Its storage depends on a solver state,
  so the current register plan cannot assign a single fixed layout. Existing
  [matrix callback fixtures](../../tests/fixtures/matrix_callback_contexts_fallback.stan)
  exercise this refusal across all five families, including calls inside runtime
  branches. [Tests](../../tests/test_callback_geometry.py) assert that the parent
  output graph can remain compiled while just the callback uses MIR.
- **Incompatible branch/return shapes, unsupported expression or statement
  forms, and excessive inlining.** The shared
  [register compiler](../../runtime/include/stanli/mir_prog.hpp) checks these
  contracts. It has an inlining-depth limit of 32 and a register cap of
  1,048,576. General runtime recursion is not implemented as compiled call frames.
  Some bounded calls can be inlined; “contains a function call” does not imply MIR.
  MirInterp itself also has a recursion-depth guard (`udf_depth_ > 64`), so its
  fallback does not provide unrestricted recursion either.
- **Compile-time evaluation that cannot safely determine admission.** Callback
  compilation can refuse after a domain error in a speculatively considered
  expression, leaving evaluation to the actually executed path. See
  [callback compilation](../../runtime/src/ode_prog.cpp#L200).

Runtime loop bounds, nested `for` inside `while`, early returns, matrix/nested
callback arguments, and fixed-shape runtime indices are **not blanket gaps
anymore**. PR #407 added their bounded coverage. Dynamic extents also need
case-by-case treatment: existing graph machinery already handles some bounded
runtime extents; this report does not claim every dynamic shape is interpreted.

### Entire output programs

[Output lowering](../../runtime/src/lower.cpp#L746) records the first refusal and
retains a partial graph for diagnostics. Drivers then select `WaInterp`, which
starts the complete output program again from statement zero on each call. It
does not execute a fast prefix followed by a slow suffix.

This is a particularly sharp performance boundary: an unsupported RNG overload
late in generated quantities can also move otherwise compiled transformed-
parameter work onto MIR. The existing
[partial-fallback test](../../tests/test_write_array.cpp#L1403) uses
`binomial_rng` with an integer-array trial-count argument to assert that behavior.

[WaInterp](../../runtime/src/wa_interp.cpp#L212) handles parameter reads, output
serialization, RNG state, and higher-order calls through hooks. The higher-order
hook [constructs a solver payload and attempts callback compilation](../../runtime/src/higher_order_eval.cpp#L177)
when the interpreted solver expression is evaluated. Thus an interpreted output
program can still have a compiled inner callback, but can also repeat callback
preparation on subsequent output evaluations. Moving the outer program onto its
existing prepared path can remove both costs.

The C API and BridgeStan also run up to three scratch-RNG discovery probes when
attaching this fallback. If none succeeds, the host disables interpreted output
generation and reports the failure. An attached fallback in a compile manifest
therefore does not prove that output generation is available. See
[C API selection](../../runtime/src/capi.cpp#L139) and
[BridgeStan selection](../../runtime/src/bridgestan_abi.cpp#L331).

### Standalone functions

The [function API](../../runtime/src/function.cpp#L203) tries to cache a register
program and [uses MIR when that plan is refused](../../runtime/src/function.cpp#L523).
Its remaining restrictions are more specific than “user functions are interpreted”:

- Direct arguments/results support scalar integers/reals, vectors, row vectors,
  matrices, and one-dimensional arrays of scalar integers/reals. Nested arrays
  and arrays of vectors/matrices still need a public-storage-order adapter, even
  though retained solver callbacks now have broader shape adapters.
- Results whose shape changes with real argument values cannot share the present
  fixed-layout plan. Void functions also fail the typed-return admission check.
- Plans specialize on complete dimensions and integer argument values. The cache
  holds at most eight entries with a 4 MiB accounted-storage budget; this is not a
  process-memory cap. After saturation, a missing signature initially uses MIR;
  a repeated pending signature can promote to compilation. Oversized keys/plans
  and incompatible integer/real input mirrors also refuse.
- Body admission still depends on the register compiler. Standalone compilation
  installs no solver-lowering, target, or RNG hook. Some such functions remain
  unsupported even by the fallback; the unseeded standalone RNG test expects an
  error. Printing and rejection, however, already have compiled support.

These distinctions have [existing tests](../../tests/test_function.cpp#L170)
for nested containers, changing results, cache churn, effects, and recursion.
Execution errors from an admitted plan propagate without falling back and
repeating effects.

### Preparation and initialization

The lowering object owns a [data interpreter](../../runtime/src/lower_internal.hpp#L794).
It executes transformed-data statements, including permitted RNG/solver calls,
and supplies concrete values for shapes, bounds, and folding. Canonical input
hydration already has a direct path, and the prepared environment is reused for
output lowering; input reconstruction and transformed data are not needlessly
replayed for both graphs. See [prepare_data](../../runtime/src/lower.cpp#L282).

Lowering also uses [pure-expression probes](../../runtime/src/lower_expr.cpp#L1365)
and [structured-loop probes](../../runtime/src/lower_structured_loop.inc#L35).
The latter include a separate data-UDF probe with a 4,096-statement budget and
bounded-value analysis through the existing data interpreter. These are compiler
services, not execution engines embedded in each loop iteration.

[InitInterp](../../runtime/src/init_interp.cpp#L90) always uses MIR when invoked.
It validates supplied values, interprets the initialization section, evaluates
bounds that may reference previously read parameters, and calls the existing
`unconstrain_leaf` numerical routines. Replacing this would principally mean
preparing the surrounding control/data movement; the transform mathematics
already has an implementation. Merely attaching InitInterp during compilation
does not execute it.

## Coverage work worth investigating first

This ordering is based on concrete reuse opportunities and the cost amplification
of a fallback. We do not yet have current workload-frequency measurements to
rank their total user impact.

| Candidate | Current gap and existing machinery | Conditions for keeping a change |
| --- | --- | --- |
| Fixed-size container RNG arguments | The graph rejects container forms of the newer scalar families, such as `gamma_rng`, and integer-array arguments such as `binomial_rng(trials, p)`. Register-region scalar RNG calls require scalar operands. Extend their existing `OP_RNG` path. | Match Stan's validation order, broadcasting, empty/singleton behavior, seeded draws, and subsequent RNG state. Measure complete output work, not just the draw helper. |
| Graph integer operations using existing register instructions | Named runtime integer `divide`/`elt_divide` still explicitly refuse in graph lowering, while `Program::IDIV` exists. Some runtime integer sums/extrema still refuse when their shape, initialization, or range proof is missing. | Route eligible fixed-shape work through the existing program machinery; preserve integer division, overflow/exception behavior, and evaluation order. Do not simply remove proof checks or use floating-point division. |
| Standalone container adapters | The standalone API refuses nested/container views already understood at retained callback boundaries. | Reuse the shape/storage utilities while preserving the public DataMap order, empty extents, overload selection, and per-call allocation/memory costs. |
| Narrow register admission gaps in hot callbacks | Some fixed-shape builtins or statement forms still refuse. For example, structured inverse transforms explicitly refuse inside register regions. | Identify a real repeated callback first, confirm that its MIR route actually supports the operation, and reuse existing kernels/transform routines with the required derivative contract. |
| Repeated standalone integer specialization | Changing integer values can exhaust the small cache even when they do not change storage. | Separate geometry-defining integers from ordinary runtime integers only with a semantic proof; show reduced churn without harming stable calls or enlarging every plan. |

Sources for the first two rows:
[graph RNG admission](../../runtime/src/lower_funapp.cpp#L648),
[register RNG admission](../../runtime/include/stanli/mir_prog.hpp#L2426),
[container RNG regression](../../tests/test_rng_families.cpp#L212),
[graph integer division](../../runtime/src/lower_funapp.cpp#L1127),
[register integer division/remainder](../../runtime/include/stanli/mir_prog.hpp#L3611),
[integer reduction refusals](../../tests/test_write_array.cpp#L4806).
Structured inverse refusal is in
[transform_call](../../runtime/include/stanli/mir_prog.hpp#L2711).

Genuinely changing storage and general recursive call frames are larger design
questions. Preparation and initialization should move up the list if profiles
show startup or repeated unconstraining dominates. Do not introduce another
equally slow evaluator simply to remove a MIR counter.

For each proposed migration, first establish the selected engine and measured
native cost, then compare against independent CmdStan behavior and unrelated
canaries. Measure preparation, first call, repeated calls, complete inference or
output generation, memory, and binary size as relevant. Keep correct refusal
outside the proven contract.

## Things that are not MIR fallbacks

An admitted register callback can run with Stan Math autodiff values rather than
generated reverse instructions. A graph kernel can build a local Stan Math tape.
Large constant-loop unrolling can consume preparation time and register storage.
These may be expensive, but removing MirInterp does not fix them. The previous
[loop experiments](2026-09-28-loop-history-followup.md) did not establish a
production replacement without startup/memory costs; their prototypes were removed.
Native instruction generation and stencil JIT remain tabled.

Some solver **call-site** limitations are also distinct from callback fallback.
Runtime integer arguments and solver controls added for generated quantities do
not imply general active-log-density support. DAE initial/output times still need
preparation-time values. Failure to lower those inputs can reject a model or the
outer output program, rather than merely select an interpreted residual. See
[DAE call lowering](../../runtime/src/lower_higher_order.cpp#L450) and
[ODE argument classification](../../runtime/src/lower_higher_order.cpp#L1374).

## How to observe the remaining uses

Use `dump_ops model.tmir.sexp data.json --execution-json` to inspect the final
selected structure. Callback records expose `value_engine`, `refusal`, and
`fresh_interpreter_per_call`. `STANLI_EXECUTION_REPORT=1` adds host output selection
and standalone-function diagnostics through the normal diagnostic sink.

These are not performance profiles. Counts describe interpreter constructions
and selected probe entries; nested user calls create additional interpreters.
They do not count MIR instructions or measure exclusive time. Static callback
sites also do not say how often a branch or solver executes. For a runtime
inventory, scope `ExecutionTrace` around the relevant calls, separately per thread,
and combine that evidence with native phase timings. See
[diagnostic contracts](../../docs/hacking.md#layout) and
[report implementation](../../runtime/src/execution_report.cpp#L243).

`STANLI_NO_INTERPRETER=1` only rejects models with recorded compilation fallbacks.
It does not eliminate preparation or initialization uses and is not a guard on
standalone function calls. The strict `--forbid-mir` diagnostic observes even
interpreter construction during preparation, so failing it does not demonstrate
a runtime performance cliff. Its current CLI scope is model compilation, not
every future evaluation of that model.

Eventually deleting MirInterp would require covering all six roles above,
including supported errors, effects, shapes, recursion, and initialization,
then replacing the interpreter-dependent test oracles. Tests that intentionally
force interpretation (`STANLI_WA_FORCE_INTERP`) are validation uses, not normal
production selection. Removal should follow demonstrated coverage and native
performance improvements; it should not be the optimization target itself.
