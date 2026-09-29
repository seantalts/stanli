# Compiler, layout and preparation research

Read the [contributor map](../../hacking.md), [lowering walkthrough](../../lowering-walkthrough.md)
and [optimization contracts](../../../runtime/src/OPTIMIZATIONS.md) for current
code ownership and eligibility. Historical plans describe how those contracts
evolved; verify completion in source rather than executing old task lists.

| Area | Record and what it contains |
| --- | --- |
| Frontend/vectorization | [stanc3 lessons](../archive/plans/2026-08-25-stanc3-vectorize-lessons.md), [MIR rollout](../archive/plans/2026-08-26-ocaml-mir-backend-rollout.md): upstream integration and producer/consumer boundaries. |
| Register regions and kernels | [Native adjoint plan](../archive/plans/2026-08-08-native-adjoint-program.md), [CALL design](../archive/plans/2026-08-09-kernel-call-instruction.md), [loop-bearing regions](../archive/plans/2026-08-29-loop-bearing-regions.md): differentiation and fallback contracts. |
| Shared lane/layout proofs | [Lane-layout unification](../archive/plans/2026-09-11-lane-layout-unification.md): read its review amendments; [carver boundaries](../archive/plans/2026-09-11-carver-boundaries.md), [generality](../archive/plans/2026-09-09-vectorize-consumption-generality.md). |
| Storage and copies | [Destination forwarding](../archive/plans/2026-08-28-destination-forwarding-results.md), [idata compaction](../archive/plans/2026-08-30-idata-compaction-results.md), [scratch layout](../archive/plans/2026-08-30-op-scratch-layout-results.md), [shared data/pullbacks](../archive/plans/2026-09-11-shared-data-native-pullbacks.md). |
| Preparation | [Preparation tuning](../archive/plans/2026-09-10-prep-time-tuning.md), [teaching investigation digest](../archive/plans/2026-09-15-teaching-performance.md): cold work, speculation cost and completed-graph profiling. |
| Identity slices | [Full-range slice regression](../../../notes/performance/2026-09-22-identity-slice.md): why unconditional aliases could regroup adjoints, and the narrower ownership proof. |

A logical view is not just a flat element count. Shape, storage order, activity,
control/effect dependence and ownership must survive a rewrite. Speculative
refusal must restore all compiler state. Ordinary small models can regress from
extra preparation even when a stress case wins. The [portable schema](../../../compiler/portable_ir/SCHEMA.md)
and [compiler test guide](../../../tests/compiler/README.md) own interchange and
native/JavaScript parity details. Loop retention and numerical fidelity have
separate [loop](loops.md) and [numerical](numerics.md) guides.
