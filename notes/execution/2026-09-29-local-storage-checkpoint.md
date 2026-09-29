# Next boundary: runtime-sized local storage

The direct RNG, integer-expression and standalone-container changes are
implemented and measured in the [working checklist](2026-09-29-execution-coverage-checklist.md).
The next storage change needs a design review. This note proposes a concrete
scope; it does not introduce another execution engine or ship an experiment.

## What the small experiment says

The existing `gq_partial_fallback` fixture calls a helper that creates either a
one-element or two-element local array, fills it, and returns its sum. That
local declaration currently forces the entire output block through MIR,
including otherwise-supported binomial draws.

A source-level equivalent with separate fixed-size branches already compiles.
On the integrated native Release build at `d6449fbf` (upstream `d13f7fa9`), six
alternating fresh-process pairs gave these median output-row times:

| Number of following binomial outputs | Whole-output MIR | Fixed-storage control | Removable difference |
| --- | ---: | ---: | ---: |
| 1 | 7.57 µs | 0.61 µs | 6.97 µs |
| 32 | 9.89 µs | 2.34 µs | 7.55 µs |
| 1,024 | 69.61 µs | 53.36 µs | 16.25 µs |

Preparation was 289→295, 288→298 and 419→392 µs, respectively. Row-time
MADs were 0.19→0.01, 0.17→0.03 and 1.07→0.81 µs. Peak process RSS was
approximately 28–29 MB. The [measurements](data/2026-09-29-local-fallback-opportunity.json)
retain samples, sources, inputs, build identity and compact parity records.
Gradients and full seeded output rows matched exactly at three input points for
each size. This is cross-path comparison, not a new independent CmdStan oracle.
No inference timing is claimed: this particular fixture has improper flat priors.

These are **potential savings from direct compilation**, not measurements of a
local MIR call. A new boundary would consume part of that saving. This is a
synthetic refusal fixture, not evidence that the same cost dominates a real
application. The ordinary recorded corpus was almost entirely compiled already.

## Recommended next design

First extend the existing structured-region machinery to a **closed, pure block
with bounded local storage and fixed-size external results**. A closed block
owns a temporary and every operation that needs its changing length. Only its
fixed-size result leaves the block. For the example, allocate two elements once,
keep the current length as a separate value, and return one scalar.

This uses support already present in `lower_structured_loop.inc`: proved
capacity, a snapshot of the declaration's extent, logical-length-aware kernels,
checked dynamic indexing and private fixed storage. It broadens where that
support may be admitted; it does not require a new general evaluator.

Do not merely enable runtime declarations in ordinary graph lowering. Some
ordinary indexed reads/writes, whole-variable bindings and output layouts still
assume that allocated capacity is the actual shape. A two-element allocation
must not make index 2 legal when the current array has length 1. Also, the O1
compiler inlines the example helper: a fallback confined to retained function
calls would miss it.

Proposed first contract:

- Admit only a region whose local capacities have finite, checked proofs and
  whose externally visible results have fixed type, shape and ownership.
- Preserve declaration-time extent evaluation, lexical scope, initialization
  sentinels, bounds errors, assignment shape checks and error placement.
- Keep changing extents inside the region. Prove all length-sensitive consumers
  use the logical extent, including constant indices and empty arrays.
- Start with value-only generated quantities and pure operations. RNG, messages,
  target updates and solver callbacks remain outside this first region; supported
  surrounding work resumes normally. An exception propagates once, with no MIR
  retry or duplicated preceding effect.
- Select the region before building competing expensive candidates. Keep the
  current path for already-supported ordinary blocks, and refuse transactionally
  when the complete region is not supported.
- Set an explicit preparation/memory admission policy for capacity. A loose
  bound that reserves a huge arena for a usually tiny array is not automatically
  a performance improvement. Benchmark small/medium cases and stress bounds.

## Review questions and evaluator

The design decision is whether to admit these closed blocks into the existing
structured engine before adding a local-MIR call contract. I recommend that
order because this concrete refusal has bounded storage and existing numerical
operations. Genuinely unbounded storage and recursive call frames remain separate.

The implementation proposal should specify the region selector, capacity budget,
input/output binding and rollback state before changing admission. Start from
semantic liveness and shape proofs, never the example's function or variable name.

Validate changing lengths across repeated calls, zero/one/max extents, negative
and excessive extents, mutation of the size variable after declaration, partial
initialization, invalid constant and runtime indices, nested control and early
returns. Check exact RNG/error ordering around the pure region and independent
CmdStan values. Compare whole MIR, bounded direct execution and the fixed-storage
control, including tiny callers, repeated calls, startup, complete outputs and
memory. Include ordinary graph/register/loop canaries.

If direct admission fails for a real hot case, the next experiment is a typed
local-MIR call around the smallest closed region. It must define packing,
lifetimes, private invocation state, fixed result validation, diagnostics and
strict no-interpreter behavior. Merely calling MIR from a kernel is not that
contract. Do not implement both mechanisms at once or keep an unmeasured backend.
