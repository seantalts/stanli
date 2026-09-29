# Fable review: local fallback and long-term coverage

September 29, 2026. User-requested read-only Claude CLI review, version 2.1.284,
requested model `fable`, resolved model `claude-fable-5-1`, high effort.
The CLI completed successfully (`is_error=false`). Source base:
`df86223160f1387a37ed47c267df1bf31e77f11c`, plus the uncommitted remaining-use
report. Local prompt, streamed transcript and result are retained under
`.cache/fable-local-fallback-strategy/` (not committed). No implementation or
benchmark was run. The [strategy](2026-09-29-local-fallback-and-coverage-strategy.md)
incorporates the review with the corrections below.

## Disposition and source corrections

Accepted: reuse the existing interpreter behind a narrower region boundary;
check the potential region size before investing; retain direct native coverage
as a comparator; report fallbacks per case; treat differentiated callbacks as
a distinct, high-amplification problem. Dynamic storage/call frames remain a
separate design decision with a trigger: measured hot fallback work that ordinary
handler/admission fixes cannot remove.

Corrections to the review, verified against executable source rather than comments:

- Runtime integer live-outs already cross islands. `lower_runtime_ifelse` preserves
  their integer type, places them in runtime graph slots, and removes stale
  compile-time facts ([lower_stmt.cpp](../../runtime/src/lower_stmt.cpp#L630)).
  Their use in a later runtime-sized declaration can still require a larger region.
- The comment near `lower_stmt.cpp:320` is stale. The register compiler emits
  PRINT/REJECT ([mir_prog.hpp](../../runtime/include/stanli/mir_prog.hpp#L4618));
  PRINT executes only on the double pass
  ([program.hpp](../../runtime/include/stanli/program.hpp#L882)). A new MIR
  derivative boundary still needs its own explicit effect/replay contract.
- Existing calls and island analysis are reuse points, not proof of zero new
  boundary work. Fixed buffer sizes, six input ranges, logical layouts, mutable
  call state, effect classification and backward lifetimes still require design.
  “Concurrency safe by construction” holds only if all mutable state and host
  resources are private or correctly synchronized, including nested calls.
- The straight-line graph schedule places runtime control inside callable
  kernels/regions. That is a limit of the flat schedule, not an inherent inability
  of the existing graph-plus-regions framework to express runtime control.
- Identical graph dumps are a useful structural regression check, not the only
  valid check and not proof of unchanged startup, memory, or runtime performance.
  Ratchet existing cases and their selected routes, not raw corpus-wide fallback
  counts: adding support for a previously rejected model may legitimately add
  interpreted regions. Statement counts are only an opportunity heuristic.
- A value-only first stage does not improve active callback fallback. This is not
  evidence that a later differentiated local region could never improve a
  previously wholly interpreted callback. That benefit remains unmeasured.

The audit now explicitly records MirInterp's own recursion-depth guard. No
runtime code or source comments were changed during this review.

## Fable's returned review (verbatim)

Verified: the interpreted output program allocates a fresh solver spec and recompiles the callback on every evaluation (higher_order_eval.cpp:260 and :290), so the audit's repeated-preparation claim holds. Report follows.

**Conclusion.** No expression-level escape into MIR exists today, and a local boundary is feasible because the necessity-island lowering already computes the live-in/live-out contract a MIR region needs. The provisional plan is practical and does not create a second interpreter: the region evaluator would be the existing `MirInterp`, scoped narrower. What it does create is a new adapter and a comfortable fallback that can stall handler closure. I would keep the plan, add a corpus ratchet and a compile-only pre-check, and state plainly that a value-only region does nothing for the highest-amplification cliff, which is differentiated callbacks. Dynamic storage and call frames stay deferred, but with a named trigger.

## Current behavior and what a local boundary needs

Current behavior, verified in source:

- **Callbacks are all-or-nothing.** Each of the five solver families picks register or interpreter for the whole function on one flag at ode.cpp:60. The choice is made at preparation.
- **Output programs restart from zero.** The comment at lower.cpp:766 says there is no continuation frame for a nested failure or its lexical live-outs. One refused statement moves the entire section, including already-lowered transformed parameters, onto the interpreter.
- **Log density has no interpreter fallback.** Parameter-dependent control becomes a necessity island. If the register compiler refuses, lowering fails at lower_stmt.cpp:477 and the model does not compile.
- **Interpreted output programs redo callback preparation per draw.** Verified above.

Feasibility. The island lowering in `lower_island` (lower_stmt.cpp:313 onward) already handles a statement region or a ternary expression: it collects assigned names, records live-out views, a target flag, and an effect flag. A MIR region is the same region contract with a different payload holding the MIR subtree plus live-in names, and a kernel forward that binds the interpreter environment from the input descriptors, runs, and copies live-outs back. Because `Program::CALL` can invoke any graph kernel (program.hpp:181) and structured loops have kernel-call nodes, one region kernel is reachable from all three engines without new boundary code. That is the design, not a fourth engine.

When a single expression suffices: a pure or effect-only expression whose result shape is provable at preparation and whose operands are graph values. Most builtin-overload gaps, such as container RNG forms, fit here.

When the region must grow:

- **Writes.** An unsupported index form on assignment makes the whole outer variable a live-out; the slot is overwritten wholesale. Correct, conservative, and it blocks CSE across the region.
- **Integer live-outs.** Live-outs are registers holding doubles. Integers leave a region only through the compiler's folded environment, and only when they certainly happen (lower_stmt.cpp:456). A later declaration sized by a runtime integer the region produces must sit inside the region, which then extends to the end of that variable's scope. Top-level outputs are fixed-size by the column contract, so block locals are the risk.
- **Loops.** A break or continue under a runtime condition already promotes the whole enclosing loop (lower_stmt.cpp:259). A MIR region follows the same rule.
- **Functions.** Recursion, or a return whose shape depends on real argument values, encloses the whole call.

Effects and evaluation order. The graph runs ops in emission order. Constant folding checks the effect flag and `island_has_effect` before touching an island (constfold.cpp:62). A MIR region needs the same flag. I verified only that pass; the others must be audited rather than assumed. Within-statement effect order inherits the existing RNG ordering contract if the region executes at its source position.

Exceptions propagate like any kernel with no rollback, matching CmdStan. Concurrency is safe by construction since each call constructs a fresh interpreter, but that is allocation-heavy; a per-executor kernel state could cache an environment skeleton under the clone contract at kernel_types.hpp:19.

Derivatives are the blocker for log density. Islands differentiate by var replay under nested autodiff (island.hpp:13). A MIR region would replay `MirInterp<var>`, which means interpreting twice plus tape memory. The comment at lower_stmt.cpp:320 records that print and reject are refused in register regions because replay repeats them. A log-density MIR region has the identical problem. So value-only output generation is the right first target, and a log-density region is a correctness backstop, never a performance path.

Why catch-and-restart in MIR is unsafe. The rule at function.cpp:532 says never repeat effects through a fallback. The reasons: the RNG stream is caller-owned and already advanced (kernel_types.hpp:27); output slots and the target accumulator are partially written; CmdStan treats the exception as a rejection, so masking it changes sampler behavior; and an exception from an admitted program is semantics, not a coverage signal.

Classification of the gaps:

- **Representation limits, patchable.** Graph refuses container RNG forms at lower_funapp.cpp:655 and runtime integer division at :1134 while `Program::IDIV` exists. Seven-argument functions exceed the six-input op, which islands already work around by packing. Register regions refuse container RNG operands at mir_prog.hpp:2442 and structured inverse transforms at :2715. Standalone functions lack nested-container adapters. Back edges lose the generated reverse (adjoint.cpp:57) and fall to var replay.
- **Inherent to graph.** Runtime control flow. The graph is a DAG; control lives only inside kernels.
- **Limits of the current register machine, not of register programs.** Unbounded storage and recursion. The file is fixed at `kMaxRegs`, functions inline to depth 32, and there are no frames. Stan sizes are fixed at declaration, so "dynamic storage" means per-activation sizing. A boxed register kind plus a frame stack is a bounded extension, not a general dynamic language.
- **Effects.** Already representable in all three engines.

## Approaches and the strongest objection

- **Patches only.** Necessary but never sufficient. Categorical gaps stay open, and every unpatched gap remains a section-wide cliff.
- **Opaque MIR region kernel.** Preferred. Turns a section-wide cliff into a region-wide one, reuses the interpreter, composes with all engines.
- **Dynamic values and call frames in Program.** Closes categorical gaps inside the fast engine. Costs a dynamic-check tax on every instruction or a second instruction family. Deferred per the decision note, with no measured trigger yet.

Strongest objection to my preference. Each region evaluation builds value vectors and a string-keyed environment. A region inside a loop reached through a call instruction pays that per iteration and can lose to the whole-section interpreter, which keeps one environment. The lowering change from section-granular to statement-granular snapshot and restore touches shared code; the only acceptable proof of no effect on fast models is bitwise-identical `dump_ops` output on every untruncated corpus model. Passes must treat the region as opaque and effectful, which pessimizes an otherwise foldable section in any model that uses one. And the region does nothing for differentiated callbacks.

Does it merely defer a second interpreter? No. It is one interpreter with narrower scope. What it defers is the decision on dynamic values, and that is correct if the trigger is explicit: a region in a hot loop that no handler can close.

## Stages, first experiment, audit corrections

Proposed stages:

1. **Define coverage as a matrix.** Enumerate stanc3 signatures by context, engine, and need, where need is value, gradient, or effect. Run a compile-only sweep of single-statement probe models and classify each as graph, register, interpreter, or failure. Rank by corpus frequency times amplification.
2. **Close handlers by amplification.** Callback register admission first, then output RNG containers, graph integer ops, standalone adapters.
3. **Region kernel for output generation.** Value-only, reported through the execution report, ratcheted in CI: the count of truncated output programs and region ops across the corpus may not rise without a linked handler issue.
4. **Log-density region only as a var-replay backstop.** Effects excluded, numeric oracle gated, with a slow-region diagnostic.
5. **Decide dynamic values from evidence.**

Throughout, the CmdStan corpus replay at the documented ULP gate stays the oracle, and region output must match graph output through the existing cross-path harness that `STANLI_WA_FORCE_INTERP` enables.

What a general fallback cannot guarantee: performance, since tree walks and name lookup amplify inside loops and solvers; coverage of builtins the interpreter itself lacks, because its vocabulary is Stanli's registry rather than all of Stan Math; differentiated effects; and binary size, since every new kernel signature is template growth.

First experiment, as a proposal only. Fixture: the existing partial-fallback test at test_write_array.cpp:1404, where transformed parameters lower and one integer-array `binomial_rng` truncates the section. Comparators: the current whole-section interpreter, and a handler-patched container draw as the upper bound. Correctness: seeded draw sequence and post-draw RNG state must match both the interpreter and CmdStan. Pre-check before building the kernel: for every currently truncated corpus model, compute what fraction of section statements the smallest closed region would swallow. Continue if region cost sits nearer the handler bound than the whole-section cost, canary graphs are identical, and the oracle passes. Reject if marshaling makes the whole section faster at ordinary sizes, or the pre-check shows regions swallow most of their sections on real models.

Audit corrections. No substantive error in the claims I verified. Two citation drifts: graph integer division refuses at lower_funapp.cpp:1134, not the function header, and `transform_call` begins at mir_prog.hpp:2712. One omission: the interpreter caps user-function recursion at depth 64 (mir_interp.hpp:148), so "MIR handles recursion" is bounded too, and CmdStan's limit is the C++ stack. The audit should say so when it lists recursion as a fallback role.
