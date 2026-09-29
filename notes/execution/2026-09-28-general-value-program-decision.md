# Deferred general-value runtime proposal

**Decision: deferred; no prototype implemented.** Prevent performance cliffs by
extending the existing graph, register and structured-loop engines first.
Replacing MirInterp with another equally slow evaluator is not a success.
Native performance is the priority; native generation and stencil JIT remain
tabled. The [current execution roadmap](2026-09-28-execution-engines-and-roadmap.md)
records completed coverage and genuine remaining gaps.

## What the proposal would change

Compile remaining value-only MIR into reusable typed instructions over
runtime-owned scalar/container values, logical shapes and explicit function
frames. Numbered slots and resolved functions could avoid repeated tree walks
and name lookup. This would still dispatch instructions in C++; it would not
remove all interpretation or the Stan Math tapes used inside some kernels.
Keep fixed-storage graph/register paths for work they already run efficiently.

Dynamic shapes, recursion/call frames and partial-environment preparation
exceed the current flat-register contract. A separate representation was a
provisional way to avoid taxing every existing instruction with dynamic checks,
not a selected architecture. Extending Program itself remains a comparator.

## Evidence needed to reconsider

Start only when a measured remaining MIR route causes a meaningful native
cliff. A bounded experiment would compare alternating result lengths, a bounded
recursive helper, changing temporary extents during preparation, and an ordinary
fixed-shape canary. Use existing builtin/Stan kernels; begin with double values.
Callback autodiff, inverse transforms and complete language coverage would
remain unclaimed.

Preserve integer results, exact effects/rejection order, shape/storage order,
view lifetimes, aliases, and unknown-input refusal. Reuse prepared work so
short-lived folding probes do not pay fresh compilation repeatedly. Failure
must leave speculative state unchanged. Compare against MirInterp and independent
CmdStan outputs, including nonfinite classifications and subsequent RNG state
where relevant.

Measure preparation, first and warm calls, specialization churn, retained/peak
memory and binary size. A speedup in a repeated function is not evidence of
model-gradient throughput. Require a useful measured benefit and no resolved
ordinary-model regression before choosing a larger stage. The original
[Fable architecture review](2026-09-28-fable-architecture-review.md) records
additional requirements; it does not authorize implementation.

Deletion would still require all builtins/container operations, effects/RNG,
initialization, dynamic outputs, partial-input preparation, and differentiated
callbacks. Keep correct fallbacks until a build with production interpreter
entry points disabled passes those contracts.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/notes/2026-09-28-general-value-program-decision.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
