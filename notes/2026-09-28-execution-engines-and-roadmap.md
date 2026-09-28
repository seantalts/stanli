# Execution engines and a path to interpreter independence

> **Native performance update:** normal_id_glm now records Stan's analytical
> partials without a nested tape. Small Gaussian gradients improve 27.6% in the
> measured native case; [results and limitations](2026-09-28-normal-glm-results.md).


> **Progress update:** Shared register compilation now handles fixed-shape early
> returns through branches and loops; see [implementation, validation and native
> measurements](2026-09-28-function-exits.md). Eligible standalone functions now
> use cached register plans; see [results and remaining refusals](2026-09-28-standalone-function-results.md).
> Earlier inventory statements below describe the audit baseline, not this
> completed return coverage. Native instruction generation remains tabled.

> **Current decision (2026-09-28):** Native instruction generation, stencil
> JIT, and dispatch-JIT research are tabled at the user's request, informed
> by their earlier negative investigations. The codegen proposals below are
> historical, not active next steps. Native performance is the priority; Wasm
> is a secondary demo. Continue coverage migration and measured improvements
> within the existing graph, register, structured-loop and kernel engines.
> Removing `MirInterp` remains an objective and does not require a JIT.
> Continued work: [remaining RNG coverage](2026-09-28-vector-rng-implementation.md).

Source baseline: `0bb5b54c8dfa7e9ee40d109209495fe7a9024508`, fetched
`origin/HEAD` → `origin/main` on 2026-09-28 and verified as an ancestor of HEAD.
On the review turn, the branch fast-forwarded to fetched `origin/HEAD`
`5b913866768f351f86eed4d5a7246414530f3b2d` without conflicts. That commit
updates benchmark reporting; the audited runtime sources are unchanged. The preceding
[function and fallback inventory](2026-09-28-interpreter-gap-audit.md) remains
the detailed source inventory.

[Fable's review and finding dispositions](2026-09-28-fable-roadmap-review.md)
record the independent review of the original plan and the revisions below.
The revised text has been source-checked locally, not reviewed a second time
by Fable.

This document records the explanation and plan. The subsequent
[first census implementation and validation](2026-09-28-execution-census-implementation.md)
records delivered work and remaining attribution limits. The subsequent
[scalar RNG migration](2026-09-28-scalar-rng-implementation.md) and
[code-generation decision](2026-09-28-codegen-decision.md) record the next
implementation and feasibility evidence. No fresh performance
experiment is claimed here. The user objective is to move more
supported Stan execution onto efficient paths and ultimately make an
interpreter unnecessary, while preserving numerical fidelity, short startup,
small self-contained distributions, and native/browser support.

**Dispatch and differentiation are independent choices.**

An instruction stored as data says what operation to perform and where its
operands live. Dispatch reads that description and transfers control to its
implementation. For the illustrative expression `z = sin(x * y)`:

```text
MIR tree:        visit call(sin) -> visit multiply -> resolve x and y -> evaluate
Graph:          call multiply_kernel(context0); call sin_kernel(context1)
Register code:  MUL r2, r0, r1; SIN-or-kernel-CALL r3, r2
Native code:    execute the generated multiply and sin call directly
```

The graph sketch describes the already-bound plan: the current hot loop does
not repeatedly resolve names or look up kernels by opcode. It walks an array
of preselected function pointers and contexts. The register sketch describes
an instruction stream whose evaluator switches on each opcode. Its registers
are numbered cells in a memory buffer, not processor registers.

All of these evaluators and kernel implementations are compiled C++ today.
What is not compiled into new machine code is the individual model's sequence
of operations. Translating MIR into a graph or register program is compilation
to a lower representation, but that representation is still interpreted.

Dispatch matters most when each instruction does little work. One indirect
call to a matrix kernel can cover substantial numerical work; thousands of
small scalar operations pay repeated dispatch and buffer traffic. Replacing
vector kernels with scalar instructions can therefore make execution worse.
Native generation can also remove intermediate loads/stores and expose more
optimization to the code generator, but does not automatically improve an
already efficient large kernel.

Sources: [graph binding and sweeps](../runtime/src/executor.cpp),
[register execution](../runtime/include/stanli/program.hpp), and
[repository architecture explanation](../docs/hacking.md).

**Current execution mechanisms and their coverage.**

| Mechanism | Representation and runtime work | Current uses and limits |
| --- | --- | --- |
| MIR tree interpreter, `MirInterp<T>` | Recursively visits MIR nodes, resolves named values, and constructs intermediate containers. `T` can be `double` or Stan `var`. | Normal execution for transformed data, lowering-time folding/admission probes, constrained initialization and standalone function calls; fallback for incomplete write-array lowering and refused retained callbacks. Solver fallback adapters construct a fresh interpreter for each callback evaluation. It is not a universal rescue for every unsupported model. |
| Graph executor | Fixed graph of slots and operations; preparation binds function pointers, contexts, scratch, values and adjoints. Forward and reverse walk the bound plans. | Main log-density path and graph-backed write-array. Broad scalar, vector, matrix, density and transform vocabulary. Control is represented by nested region/loop operations. No per-model machine-code generation. |
| Register VM, `Program` | Flat instructions over numbered scalar/range buffers, with comparisons, jumps and `CALL` into graph kernels. | `OP_ISLAND` scalar regions, parameter-dependent control, compiled solver callbacks and some generated-quantities regions. Supports runtime `while`, but compile-time `for` expansion, arbitrary early returns, shapes, selectors and register limits still impose boundaries. |
| Generated reverse program, `AdjProgram` | An ahead-of-evaluation reverse instruction stream over double values and adjoints. It is run by another dispatcher. | Many straight-line programs and acyclic branches; unsupported derivative instructions or back edges retain var replay. The flag `native_adj` does not mean native machine code. A `CALL` may still reach a taped kernel. |
| Structured loop engine, `OP_LOOP` | A reusable control tree over graph kernels, plus recorded versions, executed control, compiled recording instructions, guarded replay streams or frames. | Retains qualifying `for`/`while`/`if` structure without expanding every trip. Reuses numerical kernel backwards and stored history. Geometry, effects and embedded fallback restrictions still limit admission. The plan and replay are not a MIR tree or a general scalar Stan Math tape. |
| Precompiled kernels and Stan solvers | Ordinary compiled routines doing numerical work, called by the mechanisms above. Solver callbacks can re-enter a register program or MIR interpreter. | Shared numerical implementation across paths. A kernel can use direct arithmetic, recorded partials, a retained factorization, or a nested Stan Math tape. This is a layer inside the engines, not a competing model engine. |
| Model-specific native / WebAssembly code | Generated instructions for the model's own arithmetic and control, calling existing numerical kernels where useful. | Proposed end state; not an existing execution backend on this baseline. |

Here, model-specific native code means generated machine instructions. Existing
uses of “native” in `native_adj`, direct kernels and native `reduce_sum` workers
do not establish that such a backend exists.

The same model can use several rows during one evaluation. For example:

```text
bound graph -> OP_ISLAND -> register CALL -> a matrix kernel
bound graph -> OP_LOOP -> retained kernel call -> a density kernel
bound graph -> OP_ODE -> Stan integrator -> compiled or interpreted RHS
```

There is no defensible single percentage of “compiled coverage” for these
overlapping mechanisms. Count phase × signature × shape × activity × control
context, and separately count how often each path executes. The historical
119/119 graph-backed write-array result is one corpus result; the current
explicit RNG dispatch still has 29 interpreter names versus 16 graph names.
Successful graph construction and successful generated reverse do not imply
that every nested operation avoids a tape.

**What a taped kernel does.**

Stan Math reverse-mode variables record derivative callbacks and their needed
state on an autodiff tape as numerical operations execute. A reverse sweep
walks that tape and propagates output adjoints back to inputs. Allocation can
use an arena, and a vector/matrix callback can cover many values; it is not
necessarily a separate heap allocation or tape node per scalar.

An already compiled Stanli kernel can build such a tape internally. There are
several current patterns:

| Derivative implementation | Work and storage | Examples / caveats |
| --- | --- | --- |
| Direct pullback | Run a precompiled reverse routine using inputs, outputs or retained numerical state. | Arithmetic, matrix products, Cholesky and supported eigendecompositions; exact activity and reduction order remain observable. |
| Recorded Stan Math partials | `rvar` obtains value and partials from compatible unmodified Stan Math probability templates into double scratch; backward scales/accumulates them. No scalar `vari` tape is built for that call. | Many densities and some GLMs. The recorder cannot accept every algorithm that performs arithmetic on its autodiff scalar. |
| Generated region reverse | Compile derivative instructions once; retain only the values/control needed by the reverse program. | Generated `OP_ISLAND` adjoints; their `CALL`s delegate to whichever derivative the callee implements. |
| Nested tape in backward | Evaluate values, then reconstruct the local computation using `var`, seed outputs, sweep, and scatter input adjoints. | Generic scalar binary kernels, graph softmax/log-softmax and several matrix functions. |
| Tape once with cached partials | Construct/differentiate a local tape during forward, store partials, then contract in backward. | `normal_id_glm` and other supported wrappers. Removes a second replay but still pays for the first tape and scratch. |
| Retained structured reverse history | Save executed operations, versions, values and branch/loop history; invoke kernel backwards over that history. | `OP_LOOP`. Calling this a “tape” does not make it the same mechanism or cost as Stan Math scalar `var` replay. |

Removing dispatch does not remove an internal tape. Replacing a taped backward
does not remove graph/VM dispatch. Moreover, reverse mode will still need
saved numerical state or recomputation after scalar autodiff tapes disappear.
“No interpreter”, “no var replay”, “no repeated factorization” and “no saved
state” are four different claims; the last is generally not a useful target.

Sources: [recorder](../runtime/include/stanli/recorder.hpp),
[generic legacy backward](../runtime/include/stanli/legacy.hpp),
[island execution](../runtime/kernels/island.cpp),
[generated reverse CALL](../runtime/src/adjoint.cpp), and
[structured execution](../runtime/src/structured_loop.cpp).

**Destination and migration policy.**

The proposed destination is model-specific native code on supported native
targets, and model-specific WebAssembly in the browser, with direct arithmetic
and control around shared precompiled Stan numerical kernels. Backwards use
generated code or kernel pullbacks, retaining necessary values and control
history. Kernels remain vector/matrix operations where that is efficient.
User installation must not acquire a C++ or OCaml toolchain requirement.

Use the existing function registry, shape/activity/effect analysis and kernel
ABI as the shared semantic foundation. Do not build another independent list
of function behavior for a new backend. The current `Program` and its generated
adjoint are suitable first inputs for a bounded code-generation experiment;
that experiment does not require a whole compiler rewrite. A broader backend
will need explicit control, dynamic storage, call frames and lifetime contracts
that the current flat register representation does not completely provide.

Coverage migration and kernel improvement can proceed before native codegen
is complete. Preserve the current correct route whenever a proof does not
apply. Interpreter deletion is the final result of replacing those routes,
not a way to force coverage by refusing formerly supported models.

**Implementation sequence and completion gates.**

| Milestone | Concrete deliverable | Exit gate |
| --- | --- | --- |
| 0. Establish an execution census | A manifest of the final selected model and bound/host-selected paths, plus preparation-entry evidence and existing inclusive phase profiles. Fix the legacy ODE fallback-reporting omission. | Report mandatory interpreter uses, nested callback use, region var replay and kernel-local tapes separately; distinguish selected, eligible, executed and unavailable paths. Existing graph/gradient paths remain unchanged; tracing disabled has no added per-instruction instrumentation. |
| 1. Close bounded value-path gaps | Shared lowering for the 13 interpreted-only RNG names; resolve graph/region RNG inconsistencies; add array forms in separate slices. Compile/cache eligible standalone functions. | Each added form demonstrably avoids MIR interpretation and preserves validation, output layout and exact RNG stream position. Changed function values with the same shapes must not reuse stale constants. |
| 2. Broaden shared callback/control support | Normalize general returns to explicit exits; preserve callback logical argument shapes; add checked runtime indexing/storage and runtime call frames where needed. Reduce avoidable unrolling/register growth. Tag each item as preserving demonstrated support, unresolved current behavior, or language expansion. | Positive and adversarial fixtures for shapes, zero extents, aliasing, control and effects. Every interpreter-backed behavior shown to work has a replacement; unproven fallback behavior and unsupported-language expansion remain separate. |
| 3. Remove measured derivative overhead | Demand-driven generated reverse, direct/recorded kernel pullbacks, factor retention, and selective loop-state retention. | Measured target improvement with unchanged semantics and no unrelated small/medium-model regression. Re-profile after each representation change. |
| 4. Complete the compiled value engine | Run transformed data, shape computations, lowering-time folding/admission probes and constrained initialization through compiled programs with read/write/check/RNG/effect support. Support partial environments, bounded speculative evaluation and transactional refusal during lowering. | A build disabling production MIR interpreter entry points passes the supported-surface matrix in native and browser modes, including installation and public interfaces. |
| 5. Make native/Wasm model execution complete | Extend a validated codegen backend to control flow, kernel calls, all public model phases and dynamic storage. Cache by actual semantic dependencies and runtime/compiler ABI. | An interpreter-free configuration runs the supported surface with no tree, register-bytecode or structured-instruction fallback. Compile/startup, gradients, draws, inference, memory and size are acceptable. |
| 6. Delete obsolete engines | Remove production MIR interpretation, then remaining model dispatch engines once their replacement is complete. Retain test references only as long as useful. | Source/link checks and installed-artifact tests demonstrate no hidden interpreter dependency. CmdStan remains the independent oracle after internal references are removed. |

Run the codegen feasibility experiment described below after milestone 0 and
an initial bounded coverage slice; do not wait for milestones 2–4 to finish.
Its results determine the design and priority of milestone 5. The numbering
describes completion dependencies, not a requirement to port every taped
kernel before testing native code. Rare taped kernels can remain in a
model-interpreter-free configuration while their performance tradeoff is
measured separately.

For milestone 3, distinguish instructions with no differentiable result from
instructions with a differentiable result but no implemented reverse rule.
Integer computations used only for guards should not automatically poison an
otherwise differentiable region. Dynamic gathers still need value-dependent
scatter rules; changing flags without those rules would be incorrect. General
loop reverse needs per-iteration history, rather than extending the current
one-visit basic-block flags past their proof.

Milestone 2's callback early returns preserve demonstrated interpreter-backed
support (`f_early` in `test_ode_prog.cpp`). Matrix and array-of-vector callback
arguments need baseline fixtures before classification: program refusal does
not establish a working interpreter adapter. General recursive model lowering
is an expansion, but the standalone function API already supports recursion
up to its interpreter depth guard (`test_function.cpp` tests success, refusal
and recovery). Compiling that API must preserve its existing recursive calls.
Runtime frames are therefore a retention requirement there, or an equivalent
correct replacement must be demonstrated.

**First implementation slice.**

Extend existing diagnostics with a post-selection site manifest identifying
phase, operation/signature, logical shape, static activity, selected value
engine, local derivative mechanism, child mechanisms, and refusal reason.
Walk the final `CompiledModel` after specialization and island pricing, then
annotate binding and host decisions, including write-array column-discovery
probes. Drivers that skip such probes must say so. Read `prog.ok`, `native_adj`,
direct-RK admission/selection and retained payloads from selected objects;
do not report a discarded lowering attempt as the executing model. In
value-only phases, mark derivatives “not used”, rather than treating a false
`native_adj` flag as var replay. Distinguish an attached interpreter from a
successfully probed public output path and a disabled/unavailable output path.

A final-model walk cannot recover interpreter work already performed during
preparation. Record coarse entry events for transformed data, folding,
admission probes and host discovery separately, including speculative attempts
and their disposition. Strict diagnostic violations must survive ordinary
speculative exception handling; they cannot disappear into a normal refusal.
This instrumentation is opt-in and must not change evaluation count or RNG use.

Keep the first slice's timing inclusive, using existing profilers and static
child-mechanism lists. Parent and child totals are overlapping; never sum them
as exclusive costs. A separate RHS microbenchmark can diagnose callback cost,
but does not measure its exclusive contribution inside an adaptive solve.
Defer nested clocks, memory attribution and exhaustive executed-path counts;
add focused opt-in counters only when a dynamic choice needs diagnosis. Static
island derivative selection needs no per-evaluation counter. Ordinary timing
runs have instrumentation disabled.

Add focused regression fixtures for: preparation interpretation; constrained
initialization; standalone function calls; interpreted GQ RNG; a refused
legacy ODE RHS; an island with generated reverse; an island with var replay;
and generated reverse calling a taped kernel. Add bounded folding/admission
probes and a matrix-argument callback baseline, recording success, refusal,
shape failure or numerical discrepancy rather than assuming fallback works.
Fix legacy ODE reporting and
distinguish the existing hot-fallback policy from a strict diagnostic policy
covering every interpreter entry. The current `STANLI_NO_INTERPRETER` is not
that strict policy.

Stopping artifact: a machine-readable site manifest and phase report for
these fixtures plus representative ordinary models. This slice ends when
attribution and policy behavior are trustworthy; it does not claim a speedup.
Then choose the first coverage patch and first performance experiment from
the evidence. Existing `dump_ops`, `STANLI_PROFILE`,
`STANLI_PROFILE_PREP`, `wa_coverage.py` and the shared corpus inventory should
be extended/reused rather than replaced wholesale.

The next bounded coverage slice is the eleven missing scalar RNG names, with
the two container RNGs and array forms kept as separate patches. Append family
encodings without changing existing scalar or reserved container variants in
`rng_family.hpp`. Capture independent CmdStan rows using the existing strict
recording mode, seed/chain conventions and pinned provenance. Add dedicated
graph/program/interpreter tests for stochastic columns and subsequent RNG
state across repeated and failing calls: the ordinary cross-path harness
explicitly excludes stochastic columns from its bitwise count. Its passing
result alone cannot validate this migration. The census must show the migrated
GQ sites avoid interpretation; preparation may still legitimately use it.

**Architectural hypotheses and bounded experiments.**

| Hypothesis | Smallest discriminating experiment | Proof obligation and near miss | Continue, revise or park |
| --- | --- | --- | --- |
| Fewer coarse operations beat faster dispatch | Compare a scalar instruction chain with a fused/ranged form, same value/reverse contract, across scalar and vector sizes. | Same validation/effect order, broadcasting and accumulation. An RNG or rejecting operation between elements must not be reordered. | Continue where total bytes/instructions and time improve. Keep coarse matrix kernels as a control. |
| Native generation removes meaningful VM work | Generate straight-line arithmetic plus its existing reverse rules from one `Program`; add one acyclic branch and one shared kernel call as separate probes. Compare VM, generated code and CmdStan numerics. | Same activity, alias topology, FP grouping, domain behavior and executed branch. Adversarial changing branches and non-finite inputs. | Continue if dispatch/memory cost is materially removed after compile cost and code size. If the kernel dominates, redirect effort inside that kernel. |
| Reusing local reverse algorithms beats var replay | Select one profiled kernel; compare direct/recorded reverse with the exact current taped route on scalar, vectorized and mixed-activity inputs. | Match weighted adjoints, ties, NaNs, signed zero and broadcast reduction order. Unit-seed agreement alone is insufficient. | Continue on verified phase and model improvement. A numerically wrong fast formula is not a viable candidate. |
| Structured native loops beat trace/graph expansion | Compile a small mutable recurrence with changing branches and indexed updates, retaining reverse values explicitly. Compare existing `OP_LOOP`, VM replay and generated code. | Correct overwritten values, early exits, zero/one/many trips, effects and changing control. | Continue when first-evaluation cost, warm time and retained bytes improve together or have a justified workload tradeoff. |
| Direct lowering from typed MIR could avoid graph/VM preparation entirely | On the same tiny recurrence, prototype typed-MIR-to-code and compare its source/load cost with graph-to-Program-to-code. Developer-only generated C++ can serve as a feasibility comparator, not a shipped dependency. | Preserve all checks/effects and source scalar types. Include an unrelated array/matrix canary. | Keep as an untested counterproposal until measured. Choose it only if avoided preparation outweighs duplicated lowering and deployment costs. |

The native backend implementation choice is deliberately open. Compare a
minimal emitter with a packaged optimizing backend on the bounded program;
measure compilation latency, executable memory, supported targets, dependency
and package size, and deployment availability. A browser emitter must call
the same kernel contracts with explicit memory/ABI ownership. No claim about
backend availability or binary cost is established by this plan.

Before selecting a backend, run two separate feasibility probes. First emit
a tiny `Program`/`AdjProgram` through a developer-only C/C++ comparator using
the repository's `-ffp-contract=off` policy, no fast-math and unchanged operation
grouping. Include arithmetic, an acyclic branch and a real kernel call, with
retained scratch, weighted adjoints, exception recovery and repeated evaluation.
A slower comparator identifies remaining costs; it does not prove that every
emitter or later fusion/storage design must lose.

Second, instantiate a minimal generated Wasm module against the actual browser
artifact and invoke one numerical kernel with forward/reverse state. Determine
whether explicit imports, table access or a narrow exported trampoline is the
appropriate interface. Measure required exports and compressed size, call cost,
shared-memory growth behavior, ownership/lifetimes, exception propagation and
allowed deployment configurations. The present build exports the public C API,
not a specified model-codegen interface. webR's side-module arrangement needs
its own probe; success in standalone browser Wasm does not prove webR works.
Validate executable-memory permissions on each intended native deployment and
module-instantiation policy on each browser deployment. Unsupported deployment
modes need an explicit installation strategy before interpreter deletion; a
hosted compiler is an alternative to evaluate, not an assumed requirement.

Sources: [kernel state and context](../runtime/include/stanli/kernel_types.hpp),
[program CALL ownership](../runtime/include/stanli/program.hpp), and
[native/browser/webR build contracts](../CMakeLists.txt).

Do not default to a JIT for every tiny model. Measure the break-even count:
additional preparation divided by per-evaluation savings, including cache-hit
and cache-miss cases. Cold-path compiled coverage and optional tier selection
can precede a later decision to remove the VM entirely. The eventual
interpreter-free configuration must also handle unprofitable-to-specialize
shapes correctly; it cannot fall back silently or narrow coverage.

**Evaluator and proposed research targets.**

Correctness follows [TESTING.md](../TESTING.md), with CmdStan as independent
oracle and the current route as a local differential reference. Preserve
existing tighter contracts and investigate differences against the 10-ULP
project goal; passing scaled-error gates is not proof of that bound. Include
all output columns and shapes, rejection and print behavior, RNG continuation,
propto/activity semantics, non-finite classifications, executor reuse/copying,
concurrency, rollback after refusal and exception recovery.

For performance, use matched builds and the
[benchmark protocol](../docs/benchmark-protocol.md): six alternating pairs,
at least 200 ms warmup and 250 ms measurement, medians and MAD. Add phase
measurements for source compilation, preparation, first gradient, forward,
reverse, warm gradients, per-draw outputs, complete inference and peak/retained
memory. Record compressed and installed size plus browser assets. Per-op
profiling diagnoses; uninstrumented runs decide performance.

Select dispatch-heavy probes by recorded instruction mix, register/buffer
traffic and profiling evidence, with an unrelated structural canary. Inclusive
`OP_ISLAND`/`OP_LOOP` time alone does not prove dispatch dominates their kernels.
Choose a provisional speedup threshold after measuring removable cost and
noise; the original 1.2× suggestion had no measured basis and is not a gate.
Report cache-miss and cache-hit break-even plus setup-inclusive estimates at
both 2,000 gradients (current public tables) and 20,000 (retained v4 artifacts).
A win at 20,000 alone does not justify default selection for short workflows.
The 2,000-gradient estimate is a required startup-sensitive control, not a
substitute for measured first-fit or complete inference, nor a new definition
of the user's “very high performance” objective.
Smaller consistent wins can justify cheaper changes; a prerequisite prototype
can continue when its next bounded stage removes an identified remaining cost.

Ordinary small/medium models and matrix-dominated cases are required controls.
Any repeatable slowdown needs explanation and resolution or an explicit
workload-based selection rule. Noise is not proof of no regression. Do not
trade longer preparation or more retained state for warm throughput without
showing the total cost. There is no current performance result for a new
backend, and no basis yet for a delivery-date estimate.

The first landing keeps PR validation focused on the changed behavior plus
the repository's required native build, CTest, recorded corpus replay,
installed Python/R checks and compiler parity. Broad sweeps and extra
platforms remain targeted/on-demand or post-submit as specified by AGENTS.md.
Removal milestones additionally require explicit no-interpreter native/browser
and installed-artifact evidence before deletion, rather than relying on
ordinary tests that might quietly select a fallback.
