# Interpreter audit: baseline and current disposition

This is a compact record of the original September 28 audit. Its initial
refusals and proposed ordering were superseded by the
[current execution roadmap](2026-09-28-execution-engines-and-roadmap.md).
Read that first; use the full original inventory below only for historical
source-level reasoning.

## What the audit established

The graph, register program, and structured-loop executors can coexist within
one model. Instruction dispatch and numerical differentiation are independent
costs: a compiled graph can call a kernel that builds a Stan Math tape.
Generated register adjoints are instructions, not emitted machine code.

Interpreter deletion must count more than hot callback evaluation: model
preparation, folding/admission probes, constrained initialization, unsupported
write-array paths, retained callbacks and standalone function behavior all
matter. The ordinary no-interpreter policy was narrower than a strict ban on
every construction. Execution reports must describe the final selected path;
speculative lowering and unselected partial graphs cannot establish coverage.
Static manifests are not frequency profiles, and inclusive parent/child times
must not be summed.

## Disposition of the original gaps

| Original area | Subsequent record |
| --- | --- |
| RNG gaps and graph/register inconsistencies | [Scalar](2026-09-28-scalar-rng-implementation.md) and [vector](2026-09-28-vector-rng-implementation.md) migrations; dedicated seeded draw and continuation tests. |
| Early returns and standalone calls | [Exits](2026-09-28-function-exits.md) and [bounded plan caching](2026-09-28-standalone-function-results.md). |
| Matrix/nested callback geometry | [Matrix adapters](2026-09-28-callback-geometry-results.md), [nested arrays](2026-09-28-nested-callback-coverage.md). |
| Runtime bounds, integers, indexing and nested loops | [Bound semantics](2026-09-28-for-bound-semantics.md), [typed integers](2026-09-28-native-integer-coverage.md), [indices](2026-09-28-runtime-index-coverage.md), [nested loops](2026-09-28-nested-loop-coverage.md). |
| Draw-dependent solver inputs/controls | [Real values](2026-09-28-callback-runtime-values-results.md), [integers](2026-09-28-runtime-integer-callbacks.md), [controls](2026-09-28-runtime-solver-controls.md), with generated-quantity scope. |
| Measured local tape overhead | [Normal GLM](2026-09-28-normal-glm-results.md) and [Cholesky correlation](2026-09-28-cholesky-correlation-results.md). |

These are bounded capabilities, not complete overload coverage. Dynamic storage,
general call frames/recursion, some active-density solver inputs, and cold-path
preparation/initialization remain separate contracts. A general-value replacement
is [deferred](2026-09-28-general-value-program-decision.md). Code generation is
tabled; prototype code was [removed](2026-09-29-execution-code-cleanup.md).

For any new migration, prove semantic eligibility and refusal, use independent
CmdStan checks, preserve effects/shapes/names/rejection/nonfinite behavior, and
measure startup, repeated work, inference, memory and size. Historical graph
coverage and old benchmark binaries do not establish current execution frequency
or a new speedup. The full audit preserves the initial source locations and
unimplemented proposals at their original revision.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/notes/2026-09-28-interpreter-gap-audit.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
