# Loops, recording and memory

For mechanisms, start with the compact [static-storage digest](../archive/plans/2026-09-03-structured-loop-static-storage.md)
and [ctsem frame/recorder digest](../archive/plans/2026-09-19-ctsem-memory.md).
Then inspect current `structured_loop` code and tests; these dates describe
successive designs, not one interchangeable baseline.

The recurring distinction is between shared executable work, live bindings,
and numerical history required for reverse. Fewer records do not automatically
mean faster execution; retaining frames may improve peak memory while costing
more than a larger instruction stream. Work removed after recording cannot
recover the recording time or transient peak already paid.

| Investigation | Read for |
| --- | --- |
| [Post-392 work](../archive/plans/2026-09-20-ctsem-post-392.md) | The next bottleneck after frames, phase-separated evaluator and provenance. |
| [Recording sites](../../../notes/performance/2026-09-20-ctsem-recording-sites.md) | Where repeated recording work actually occurs. |
| [Proof packets](../../../notes/performance/2026-09-20-ctsem-proof-packets.md) | Eligibility, adversarial cases and measured consequences of checked layouts/data proofs. |
| [Remaining architecture results](../../../notes/performance/2026-09-20-ctsem-remaining-architecture.md) | Decisions about QR retention, recording/encoding, reuse and numerical alternatives. |
| [Implementation/review trail](../archive/plans/2026-09-20-ctsem-remaining-architecture.md) | Integration details, derivative lifetime tests and direct-entry control cases. |
| [Ordinary-model controls](../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md) | Unresolved preparation/RSS observations and A/A attribution; not a universal no-regression claim. |

Keep data dependence separate from derivative activity. A memoized result needs
proof about every controlling branch, selector, alias and write. Observed equal
values do not prove constancy. Preserve error/effect order, stale-generation
invalidation, retry, copy-on-write, promotion and reverse accumulation.
Runtime-length tails are not defined values: consumers must use live extents.

The September 3 flat-frame prototype and sequence-flattening attempts did not
establish enough benefit and were removed. September 19 incremental frames had
different contracts and reduced the recorded 4,000-row peak from about 10.4 to
1.52 GB; those measurements are not a promise for every loop. September 28
callback-history prototypes were also removed; see [execution research](execution.md).
Native generation/stencil JIT remains tabled. First recording, warm gradients,
preparation and retained/peak storage must be measured separately.
