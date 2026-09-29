# Execution coverage and interpreter research

Start with the [working checklist](../../../notes/execution/2026-09-29-execution-coverage-checklist.md)
for the ordered gap map, dependencies, validation gates and design checkpoints.
Use the [remaining MIR uses report](../../../notes/execution/2026-09-29-mir-interpreter-remaining-uses.md)
for the source audit after PRs #407 and #408: production entry points, concrete
remaining coverage gaps, and opportunities to reuse existing engines. The
[execution roadmap](../../../notes/execution/2026-09-28-execution-engines-and-roadmap.md)
explains graph, register and structured-loop execution, instruction dispatch,
and why local autodiff tapes are a separate cost from MIR interpretation.
The goal is predictable native performance, not deleting an interpreter at any cost.

| Question | Evidence to open |
| --- | --- |
| What should we work on next? | [Working checklist](../../../notes/execution/2026-09-29-execution-coverage-checklist.md): direct RNG/integer/container coverage first, then measured local fallbacks and explicit broader design decisions. |
| What did the container-RNG experiment establish? | [Results](../../../notes/execution/2026-09-29-container-rng-results.md): shared upstream vectorized calls, correctness and native gains. Setup tradeoff accepted for normal end-to-end use; not yet merged. |
| Where does MIR still run, and which remaining gaps are worth investigating? | [Remaining uses](../../../notes/execution/2026-09-29-mir-interpreter-remaining-uses.md): callbacks, whole-output fallback, standalone functions, preparation, probes and initialization; source evidence, not new timing measurements. |
| Can fallback be local, and how do we approach broader Stan compatibility? | [Strategy](../../../notes/execution/2026-09-29-local-fallback-and-coverage-strategy.md) and [Fable review with corrections](../../../notes/execution/2026-09-29-fable-local-fallback-review.md): proposed region boundaries, dynamic storage/call frames, derivative contracts and staged evaluation; no implementation selected. |
| Which fallbacks have been closed? | Roadmap coverage table, then its linked per-capability reports. |
| Nested runtime loops and integer-local resets | [Nested-loop results](../../../notes/execution/2026-09-28-nested-loop-coverage.md): about 111× warm-gradient gain on the measured fixture; startup/canary limits included. |
| Return semantics and standalone function caching | [Function exits](../../../notes/execution/2026-09-28-function-exits.md), [standalone results](../../../notes/execution/2026-09-28-standalone-function-results.md): fixed-shape admission, bounded specialization and churn. |
| Callback shapes and solver inputs | [Matrix geometry](../../../notes/execution/2026-09-28-callback-geometry-results.md), [nested arrays](../../../notes/execution/2026-09-28-nested-callback-coverage.md), [runtime controls](../../../notes/execution/2026-09-28-runtime-solver-controls.md). Runtime integer/control coverage is scoped to generated quantities. |
| RNG draw ordering and refusal | [Scalar](../../../notes/execution/2026-09-28-scalar-rng-implementation.md) and [vector](../../../notes/execution/2026-09-28-vector-rng-implementation.md) reports. Ordinary non-stochastic parity checks do not establish seeded RNG parity. |
| Removing local tapes | [Normal GLM](../../../notes/execution/2026-09-28-normal-glm-results.md), [Cholesky correlation](../../../notes/execution/2026-09-28-cholesky-correlation-results.md): shared Stan arithmetic, exceptional fallbacks, memory costs. |

## Ideas already evaluated

[Structured callbacks](../../../notes/execution/2026-09-28-structured-callback-results.md)
and per-call loop histories were correct in their bounded tests but slower.
[Guarded register-path reuse](../../../notes/execution/2026-09-28-loop-history-followup.md)
improved tested warm solves by 1.7–2.3× but added first-call work and memory.
All these prototypes and code-generation probes were
[removed before merge](../../../notes/execution/2026-09-29-execution-code-cleanup.md).
A general dynamic-value replacement is
[deferred](../../../notes/execution/2026-09-28-general-value-program-decision.md).
Do not recreate a removed backend merely to reduce interpreter counters.

Dynamic storage, general call frames/recursion, some active-density solver
inputs, and preparation/initialization remain distinct contracts. Verify exact
current admission in source/tests before treating a historical refusal as open.
Use the [maintained benchmark coverage](../../benchmark-protocol.md#focused-execution-path-benchmarks)
and [execution diagnostics](../../hacking.md) for new measurements. A known
24-ULP large-branch stress discrepancy remains documented; tolerances were not
widened to accept the experiments.

The next completed slice covers generated-quantity integer division/remainder
and proved array-literal reductions. See the [integer results](../../../notes/execution/2026-09-29-integer-expression-results.md)
for the independent oracle and the measured 19-row preparation break-even.
