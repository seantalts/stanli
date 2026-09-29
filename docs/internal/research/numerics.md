# Numerical fidelity and measurement evidence

[TESTING.md](../../../TESTING.md) is authoritative for actual numerical gates,
known exceptions, oracle selection and CI. The ten-ULP objective is not a
universal measured bound. [The benchmark protocol](../../benchmark-protocol.md)
defines timed phases and publication; public [benchmarks](../../benchmarks.md)
retain their recorded inputs and revisions.

| Question | Read |
| --- | --- |
| What is in the shared corpus? | [Corpus inventory/status](../../corpus-status.md), collection provenance in the [catalog](../catalog.md#corpus-and-test-guides). |
| Why can inference time differ despite similar gradients? | [Teaching investigation digest](../archive/plans/2026-09-15-teaching-performance.md), [sampler RNG parity](../../../notes/performance/2026-09-20-sampler-rng-parity.md), [startup parity](../../../notes/performance/2026-09-20-sampler-startup-parity.md). |
| Remaining issue-374 cases | [Triage](../../../notes/performance/2026-09-21-issue-374-triage.md), [refreshed measurements](../../../notes/performance/2026-09-21-issue-374-refresh.md), [numerics](../../../notes/performance/2026-09-21-issue-374-numerics.md), [scalar experiment](../../../notes/performance/2026-09-21-issue-374-scalar-experiment.md). These records have different revisions; do not mix their timings. |
| Platform-specific oracle disagreement | [Intel macOS recording](../../../notes/performance/2026-09-22-intel-oracle.md): same-platform CmdStan distinguished oracle drift from a runtime error; eigenvector outputs were not added to the gate. |
| Lightweight densities and additive constants | [Compact densities](../../compact-densities.md), [lite build](../../lite-lp.md): public numerical contracts. |
| Benchmark/corpus artifact provenance | [Artifact inventory](../artifacts/README.md), [published run manifests](../../../output/corpus-performance/README.md). |

Use CmdStan independently; matching an old interpreter alone cannot establish
Stan semantics. Preserve effects, shapes, names, rejection, nonfinite classes
and seeded stream continuation where applicable. Do not widen tolerances to
make a change pass. Distinguish numerical disagreement from different sampler
trajectories, unstable conditioning, and platform math behavior.

Historical speedups are evidence about specific experiments. Re-baseline new
work, separate preparation/first/warm/inference costs, and use bounded A/A
controls for small signals. Keep failed/capped models visible. Raw artifacts
are for targeted inspection; never load the complete reference archive merely
to understand a plan.
