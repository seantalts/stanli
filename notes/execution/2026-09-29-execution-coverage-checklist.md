# Execution coverage: working checklist

Initial source base: `df86223160f1387a37ed47c267df1bf31e77f11c`. Integrated
fetched `origin/HEAD` at `d13f7fa907eb85617aba85a9e570de0389cdae86` without
conflicts on September 29, 2026; integration commit `d6449fbf`.

Latest sync: `b2fe8596c11fd2ff7c7069195a4b8f156ecf0593`, including merged
PRs #411–#413. The [integer-array follow-up](2026-09-29-bounded-integer-blocks.md)
records the next type extension and its proof limits.

Our goal is to prevent surprising native performance cliffs while expanding
Stan compatibility. Removing MirInterp is an eventual consequence of covering
its jobs efficiently, not a reason to replace it with something equally slow.

Performance policy clarified by the user: prevent regressions in normal
end-to-end use. Individual setup phases may cost more when justified by measured
normal-use gains; report those costs and the break-even point explicitly.

This is the working order and gap map. Checked items have evidence linked below;
unchecked items are work, experiments, or explicitly marked design decisions.
It covers the known categories, not a claim that every unsupported overload has
already been individually audited. Section 1 closes that inventory gap.

Read the [source audit](2026-09-29-mir-interpreter-remaining-uses.md) for current
entry points, the [strategy](2026-09-29-local-fallback-and-coverage-strategy.md)
for boundary contracts, and the [Fable review](2026-09-29-fable-local-fallback-review.md)
for counterarguments and corrections. This checklist is the implementation
queue; those documents retain the reasoning rather than duplicating progress.

## Engines we are extending

- **Graph:** prepared operations over known storage, often calling substantial
  numerical kernels. Best when work can be arranged before evaluation.
- **Register programs:** prepared instructions over numbered storage locations,
  including branches, jumps and kernel calls. Useful for runtime decisions.
- **Structured loops:** retained loop/control structure that runs prepared
  segments and kernels without expanding every iteration during preparation.
- **MIR interpreter:** executes the compiler's intermediate representation
  directly. It remains the compatibility path for several different jobs.

These paths compose; they are not four mutually exclusive model modes. A local
Stan Math autodiff tape records derivatives inside an operation. It is not
itself a MIR fallback or another model execution engine.

## 0. Establish the starting point

- [x] Sync the task branch with fetched `origin/HEAD`; preserve existing notes.
- [x] Audit all six production MIR roles: solver callbacks, output generation,
  standalone functions, transformed data, preparation probes and initialization.
- [x] Obtain Fable's overarching review and record source-based corrections.
- [x] Preserve negative research results: stencil JIT/instruction generation
  remain tabled; removed loop/reverse prototypes are not production options.
- [x] Record the actual native build/dependency identities before new timings.
  Prior PR measurements are historical evidence, not this work's baseline.

## 1. Make the remaining coverage measurable

- [ ] Extend the existing signature/model inventories rather than start another
  disconnected function list.
  - [ ] Record overload, argument/result types, scalar versus container shapes,
    block or callback context, active inputs, selected execution path and refusal.
  - [ ] Separate supported, unsupported, untested/generator gap, numerically
    wrong, and correct-but-slow cases. A supported function name is insufficient.
  - [ ] Include error behavior, output schema, RNG/printing effects and gradients;
    a successful value-only example does not establish model compatibility.
  - [ ] Cover ordinary graph, runtime region, retained callback, standalone,
    transformed-data and generated-quantity contexts where legal.
- [ ] Refresh each relevant historical refusal against current source/tests.
  Older prose about loops, integer outputs and print/reject is not authoritative.
  - [x] Replay all 329 recorded corpus models and refresh output-path coverage.
    The only interpreted output case was an uninitialized scalar integer;
    its declaration now compiles with Stan's sentinel value. This does not
    count solver callbacks, probes, transformed data or initialization paths.
- [ ] Combine static execution reports with scoped runtime traces and timings.
  - [x] One focused selection inventory: 330 corpus cases (329 recorded), 43
    execution/solver fixtures, and scoped traces on the four corpus ODE models.
    No application callback fallback identified; unrecorded `sir` still rejects
    sampled points. Deliberate callback refusals all concern dynamic storage.
  - [ ] Distinguish construction/probe counts from actual execution frequency.
  - [ ] Identify how much otherwise-supported work each fallback pulls into MIR.
  - [ ] Keep per-case engine expectations so existing models cannot silently
    regress. New supported models may legitimately add interpreted regions.
- [ ] Update existing benchmark tools/manifests only where a new behavior or
  missing metric requires it. The existing execution fixtures do not cover
  all remaining refusals, and output generation must be timed directly.

Dependencies: the focused baseline for an item comes first; completing the
entire inventory is not a prerequisite for a small, independently proved fix.

## 2. Close direct gaps in existing engines first

- [x] **2.1 Container arguments to RNG functions — merged in #410.**
  - [x] Baseline the existing array-argument `binomial_rng` output fallback and
    newer scalar-family container refusal, such as `gamma_rng`.
  - [x] Extend the existing RNG lowering/kernel contract for proved fixed shapes;
    cover graph and register-region admission without duplicating algorithms.
  - [x] Preserve scalar broadcasting versus length-one containers, empty inputs,
    length mismatches, validation order, draw order and subsequent RNG state.
  - [x] Verify against pinned upstream Stan/CmdStan behavior, including an invalid
    later element and a following draw. MIR agreement alone is not sufficient.
  - [x] Measure complete output time, preparation and peak process memory for small and larger
    workloads; retain a genuinely unsupported fixture for fallback tests.
  - [x] Resolve the [measured preparation tradeoff](2026-09-29-container-rng-results.md).
    Accepted for normal use: the mixed-family fixture needs about three rows to
    recover added setup; ordinary canaries show no clear slowdown.
- [x] **2.2 Runtime integer expressions — bounded slice merged in #410.**
  [Results and remaining limits](2026-09-29-integer-expression-results.md).
  [Scalar initialization follow-up](2026-09-29-uninitialized-int-results.md)
  closes a separate output-declaration fallback.
  - [x] Route eligible integer division through existing integer instructions.
  - [x] Extend sums/extrema where shape, initialization and range proofs permit.
  - [x] Test negative operands, zero divisors, overflow boundaries, empty inputs,
    indexing effects and partial initialization. Never replace integer division
    with floating-point division or simply remove a proof guard.
- [ ] **2.3 Standalone function arguments and results.**
  Nested-container adapters are [tested and adopted](2026-09-29-standalone-container-results.md);
  the remaining bullets below retain distinct contracts.
  - [x] Reuse callback shape/layout helpers for nested arrays and arrays of
    vectors/matrices; preserve public data order, dimensions and empty extents.
  - [x] Audit void/effectful calls and RNG/higher-order host hooks separately from
    numerical return values; fallback does not supply every absent host hook.
  - [ ] Reduce integer specialization churn only after proving which integers
    determine storage. Keep the existing bounded-cache memory contract.
  - [x] Keep changing result shapes and recursive frames in section 6 until a
    suitable contract exists; do not pretend they are simple adapters.
- [ ] **2.4 Repeated solver callbacks and call sites.**
  - [x] Complete the initial selected-path census across all five solver families
    in targeted fixtures; sample runtime traces on corpus ODE models. This did
    not identify a real repeated refusal needing a direct handler fix.
  - [ ] Inventory refusals separately for forward ODE, adjoint ODE, DAE,
    algebraic solves and quadrature; measure callback frequency and total solves.
  - [ ] Fill fixed-shape builtin/statement gaps using shared kernels; investigate
    structured inverse transforms only with a supported, repeated-use example.
  - [ ] Audit active integer/control arguments and DAE time restrictions at the
    solver call site. These can reject the outer model, not just its callback.
  - [ ] Where interpreted output rebuilds solver specifications per evaluation,
    establish whether immutable preparation can safely be reused with changing
    inputs, correct ownership and no startup/memory regression.
  - [ ] Verify values, solver failure behavior and active derivatives separately.

## 3. Close broader Stan function and language gaps

These include features MirInterp cannot currently rescue. Priorities within this
section follow real workloads and measured impact, not headline name counts.

- [ ] **3.1 Numerical functions and overloads**, starting from the pinned
  [coverage inventory](../../docs/coverage.md#known-unsupported-forms).
  - [ ] Extended `wiener_lpdf` overloads and `gaussian_dlm_obs_lpdf`: choose an
    argument-packing/call contract beyond today's fixed input limits; avoid
    enlarging every hot operation without evidence that the cost is acceptable.
  - [ ] `hypergeometric_1F0`, `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`,
    `wiener_lcdf_unnorm`, `wiener_lccdf_unnorm`, and `gp_periodic_cov`.
  - [ ] `discrete_range_cdf`, `_lcdf`, `_lccdf`: all-integer inputs still need
    runtime validation/value support even without a differentiable edge.
  - [ ] Remaining overload, vectorization and named-transform gaps discovered
    by the inventory. Declaration support does not imply named-function support.
  - [ ] Reuse pinned Stan Math algorithms and derivatives; turn generator gaps
    into independent oracle cases before marking support complete.
- [ ] **3.2 Control, effects and observable behavior.**
  - [ ] Audit parameter-dependent truncation/support checks and exact rejection
    placement; distinguish still-open cases from recently supported control flow.
  - [ ] Fill message argument-count and nested-container/matrix formatting gaps.
  - [ ] Verify print, reject, target updates, short-circuiting, bounds errors,
    early returns, break/continue and loops across applicable paths.
  - [ ] Preserve error/RNG ordering and prevent reverse replay from duplicating
    observable effects; printing and rejection already have compiled support.
- [ ] **3.3 Missing types — major design checkpoint.**
  - [ ] Define complex values across compiler/MIR representation, storage,
    arithmetic, differentiation and external interfaces.
  - [ ] Define tuple construction, access, return values and nested composition.
  - [ ] Extend shape/layout and oracle generation consistently. These are
    end-to-end capabilities, not just new arithmetic instructions.
- [ ] **3.4 Compatibility beyond numerical kernels.**
  - [ ] Exercise model loading, data validation, initialization, constrained
    output/names, generated quantities and relevant Python/R/C clients.
  - [ ] State the supported pinned CmdStan language/library baseline. Track
    upstream additions and external C++ integrations as explicit contracts.

## 4. Contain fallbacks that remain

Do this after cheap direct closures and an opportunity measurement. The first
boundary experiment is a major design checkpoint, not a required new backend.
The [local-storage checkpoint](2026-09-29-local-storage-checkpoint.md) records a
synthetic measurement and recommends bounded direct regions first. The
[subsequent probe](2026-09-29-execution-inventory-and-bounded-storage.md) tests
that direction. The [bounded-output implementation](2026-09-29-bounded-output-blocks.md)
now admits a narrow pure grammar under explicit storage/work limits. Finding a
real hot application refusal remains open.

- [x] Probe an existing structured region around bounded storage and a scalar
  result, preserving the wrapper in MIR; check actual execution, not just admission.
- [x] Fix the discovered capacity-versus-logical-length bug in existing shape
  folding; retain automatic-engine and independent numerical regressions.
- [x] Review ordinary closed-block admission, capacity/memory policy and the
  admitted logical-length consumers before broadening root selection.
  - [x] Pure generated quantities: owned one-dimensional real arrays, bounded
    loops, single indexing, sums/shape queries and scalar external results.
  - [x] Execute those small regions with the existing structured tree path;
    recording/rebuilding a reusable execution path costs more on changing draws.
  - [x] Test refusal rollback, initialization, changing extents, bounds errors,
    independent CmdStan values, RNG continuation and multiple imports/results.
  - [x] Extend owned real vectors/row vectors, including rows/cols and Eigen
    summation, under the same limits. Correct scalar sums at capacity one and
    MIR indexed-write rejection; see the [results](2026-09-29-bounded-vector-blocks.md).
  - [x] Extend one-dimensional integer arrays with an unconditional full-fill
    proof and int32-safe partial sums, including empty/changing lengths and
    promoted sums. See the [results](2026-09-29-bounded-integer-blocks.md).
  - [ ] Extend nested containers, other construction patterns and consumers
    with their own logical-length proofs. Effects and active callbacks remain
    separate scopes.

- [ ] Find a real refusal with substantial supported surrounding work; estimate
  call frequency, input/output copying and environment setup before coding.
- [ ] Only if a costly refusal cannot use direct support: start a local-MIR
  experiment with a pure, value-only function and fixed external result shape.
  Prepare a graph/register call into MIR, then resume prepared execution.
  Do not retry after runtime exceptions.
- [ ] Specify typed inputs/outputs, layout, lifetime, private invocation state,
  error placement and continuation. Existing kernel calls are an insertion point,
  not a complete adapter; six-input/fixed-output limits still apply.
- [ ] Compare whole-output MIR, local MIR and direct lowering where feasible.
  Include tiny surrounding work, repeated loop calls and varying container sizes.
- [ ] Keep it only if total work improves without a resolved ordinary-case
  regression. Coarsen the region or defer it when crossings dominate.
- [ ] Extend independently, with proofs and measurements:
  - [ ] Multiple results, assignment/writeback, aliases and safe control exits.
  - [ ] RNG, messages and target effects with exactly-once ordering.
  - [ ] Branch/loop/function-sized regions when one expression is not closed.
  - [ ] Active inputs: backward implementation, retained values/branch history,
    tape lifetime, exception cleanup and safe nested/concurrent execution.

A value-only success does not establish faster active solver callbacks. Prefer
eliminating a hot fallback with direct support over repeatedly crossing into it.

## 5. Cover preparation and initialization

These complete the MIR inventory; prioritize earlier if startup measurements
show they dominate interactive use.

- [ ] Prepare transformed-data execution, preserving its one-time effects,
  construction RNG stream, validation and reuse by model/output execution.
- [ ] Replace pure folding/shape/admission probes with shared analysis or
  prepared evaluation where useful; preserve transactional refusal and limits.
- [ ] Prepare initialization control/data movement around existing unconstrain
  routines; support bounds depending on previously read parameters and preserve
  dimensions, ordering, Jacobians where applicable, and rejection behavior.
- [ ] Measure source compilation, preparation, first use and repeated
  unconstraining separately. Moving work earlier does not make it free.

## 6. Decide how to execute genuinely dynamic programs

**Major design checkpoint; deferred until remaining hot workloads justify it.**

- [ ] Identify values whose size changes during execution and cannot remain
  inside a fixed-interface call or use a proved bounded capacity.
- [ ] Evaluate extending existing register programs with separately stored
  dynamic handles/views, ownership and lifetime rules. Preserve cheap fixed
  numeric instructions; do not box every scalar by default.
- [ ] Design typed call frames and return handling for recursion and general
  calls that cannot be inlined; include depth/resource limits and cleanup.
- [ ] Define differentiation across dynamic values/frames, including storage
  retained until backward execution and avoiding unnecessary dense Jacobians.
- [ ] Compare this extension with larger local MIR regions and direct specialized
  kernels on real workloads, including preparation, memory and binary size.
- [ ] Require a consolidation/migration plan before adoption. Do not maintain
  another general evaluator with the same coverage and costs as MirInterp.

## 7. Improve expensive paths that are already compiled

- [ ] Profile local Stan Math tapes, register var replay, and constant-loop
  expansion separately from MIR. Rank by complete model/inference cost.
- [ ] Add shared direct derivatives only where measured tape cost warrants them,
  preserving Stan arithmetic and necessary exceptional-case behavior.
- [ ] Revisit retained loops/reverse storage only with new evidence addressing
  the earlier preparation/memory regressions. Consult the negative results first.
- [ ] Keep instruction-generation/stencil-JIT research tabled.

## 8. Gates for every landed change and eventual deletion

- [ ] Establish a before/after native baseline with build identity, sample counts
  and timing variation. Measure affected phases, ordinary small/medium canaries,
  stress behavior, peak/retained memory and relevant binary-size costs.
- [ ] Check independent CmdStan values, gradients and per-draw outputs, plus
  shape/name/error/effect contracts. Preserve current tighter gates and document
  numerical exceptions; never widen tolerances to hide a regression.
- [ ] Preserve proven fast paths. Investigate and resolve regressions in normal
  use before landing; quantify accepted phase tradeoffs. Finite canaries cannot
  prove that no possible model regresses.
- [ ] Run focused positive/adversarial tests and required CI; add broad sweeps
  only when the change requires that evidence. Keep WASM compatibility without
  making browser performance the architecture's primary objective.
- [ ] Ship only exercised, enabled code with a measured purpose. Remove failed
  prototypes; retain compact results and logs, including negative findings.
- [ ] Before deleting MirInterp, cover all six roles, supported types/control/
  effects and active derivatives; migrate interpreter-dependent test oracles.
- [ ] Audit execution diagnostics, forced-fallback hooks and callers, then verify
  there are no remaining production references before removing the implementation.

## Current stopping point

The direct container RNG, bounded integer-expression, standalone layout and
scalar-initialization slices are implemented and tested. Their linked reports
record native performance and ordinary-use canaries; none adds an execution
engine. Unchecked categories above remain open.

The [Fable review and comparison](2026-09-29-fable-next-steps-review.md) led to
an inventory, an existing-engine feasibility probe, and then the
[bounded-output implementation](2026-09-29-bounded-output-blocks.md). The current
recorded application corpus has no interpreted output graph or observed
interpreted solver callback at the sampled valid points. The narrow output
extension closes one deliberate fixture fallback, using an existing engine.
It does not establish general dynamic containers, active callbacks, or a local
MIR continuation contract. Those need separate proofs and a measured target.

Keep a separate queue for small missing-function capabilities; they need their
own semantic/oracle proof but not an exhaustive architecture census. General
dynamic storage and local MIR calls remain conditional on demonstrated need.
