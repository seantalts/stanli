# Structured loops: static-storage research digest

Historical design and measurements from September 3–5, 2026, starting after
PR #312. This digest preserves the decisions, not an implementation checklist
for the current runtime. Later frame work and current coverage are indexed in
[the loop research guide](../../research/loops.md).

## Problem and retained design

The initial retained-loop implementation versioned whole container outputs.
Repeated scalar updates therefore created quadratic history: the recorded
10,000-element case used 1.6 GB and was 326× slower than unrolling. The proposed
replacement used one executor with static storage classes:

- Retain values required by reverse kernels; reuse transient workspace when
  no alias, output, target, or active consumer can observe an old value.
- Fuse eligible indexed updates with alias assignment, using copy-on-write
  for shared/imported storage and a LIFO undo log for overwritten values.
- Prebind kernel metadata and compute activity/invariance from semantic
  dependencies, separately from derivative activity.
- Memoize pure data-only subtrees by visit and record data-only control
  decisions. Every controlling branch, bound, alias and write matters;
  matching observed values is not a proof of invariance.
- Use existing register programs for eligible straight-line segments.
  Preserve numerical accumulation order and conservative refusal.

Forward failure must invalidate partial reverse state. Tests must cover
zero/one/many trips, nested control, break/continue, aliases, duplicate writes,
active promotion, effects, retry, and multiple executors. Historical retained
versus unrolled checks used 1e-12 relative error because their target reductions
had different grouping; this is not permission to relax current gates.

## Measured decisions

| Experiment | Recorded outcome | Implication |
| --- | --- | --- |
| Record data-only control instead of restoring guard values | ctsem N=33: about 88 → 27 ms per gradient | Remove repeated work when the full control dependency is proven. |
| Release silent memo recording storage | N=400 peak RSS 1.24 → 0.95 GB; gradient 307.4 → 306.0 ms | Reclaim only storage with no surviving readers; preserve control values and invalidate cached generations. |
| Index descriptors and cold error helpers | N=33: 26.4 → 22.6 ms; N=400: 307 → 265 ms, bitwise results | Avoid repeated descriptor lookups and hot-path error-string construction. |
| Flat per-iteration frames | m1 462 → 456 µs, m4 1220 → 1140 µs; step dispatch replaced record overhead | Prototype removed; this result does not rule out later frames with different ownership/control contracts. |
| Flatten nested sequences | N=400: 265.6 versus 264.8 ms | No resolved benefit; removed. |
| Skip static index checks diagnostically | About 5.3% gradient opportunity | Not a shipping result; separate immutable descriptor checks from runtime bounds without moving observable errors. |

These measurements belong to different successive revisions and cannot be
combined into an aggregate speedup. Original source/build details and tables
are in the full record linked below. Native generation/stencil JIT proposals
in that record are now tabled.

## Coverage and numerical cautions

The historical 226 forced-mode refusals were not shipped-selector failures:
auto mode had none in that 130-model census. Runtime extents and retained-loop
iterators still exposed capacity and shape gaps. The contemporary ctsem O1
MIR had separate logical-shape and integer-region failures; one fix did not
establish complete support.

The brms investigation distinguished data-controlled extents, which could use
a bounded compile-time while expansion, from parameter-controlled slices.
Reading a slice's capacity instead of its live extent produced incorrect
`log_sum_exp`, `max`, and `num_elements` answers. Every runtime-length consumer
must honor its extent; unused tails may contain stale values, zeros, or NaNs.
Do not infer validity from a reduction whose identity happens to hide the bug.
Current support is described by current tests and the later brms report, not
these historical refusals. Forward-only output regions also required their
own parameter-dependence, shape, and effect proofs.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/docs/superpowers/plans/2026-09-03-structured-loop-static-storage.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
