# Native parallel reductions

The [native reduction guide](../../native-reduce-sum.md) describes the shipped
opt-in C++, C, Python, R and CLI surface. It is the starting point for usage;
the older specification is not a pending request to build another worker system.

Read the [integration result](../archive/plans/2026-09-18-native-reduce-sum-integration.md)
for correctness, throughput and peak-memory evidence on the pinned Stan 2.40
base. Earlier results appear explicitly under prior-base identities. The
[compact-import experiment](../archive/plans/2026-09-18-native-reduce-sum-imports.md)
explains shared immutable inputs and separate exclusive/shared gradient
publication. The [binding plan](../archive/plans/2026-09-18-native-reduce-sum-bindings.md)
covers public interface integration. The [original specification](../archive/specs/2026-09-17-native-reduce-sum-parallelism.md)
and its reviews preserve alternatives and assumptions, some later superseded.

Key constraints: avoid nested worker oversubscription, retain private mutable
worker state, share immutable model inputs through the existing ownership
mechanism, and preserve exception/retry behavior. Fixed partitions can be
repeatable even when parallel reduction regrouping differs from whole-slice
arithmetic; use the documented gates rather than claiming universal bitwise
serial parity. Small-work cutoffs and ordinary-model costs need measurements.
The runtime's serial default and the opt-in native API are distinct from an
unused experimental backend.

Raw prototype/import/integration measurements are in
[the artifact inventory](../artifacts/README.md). Browse only the relevant stage;
prototype feasibility is not evidence for every shipped workload or platform.
