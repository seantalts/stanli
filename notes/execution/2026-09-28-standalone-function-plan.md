# Compiled standalone function calls: next bounded slice

Continue the interpreter migration after shared function exits. Native code
emission and dispatch-JIT remain tabled. The public standalone function API
currently constructs a MIR interpreter for every invocation, even for an affine
vector function. It is a production entry point required for eventual deletion.

## Evaluator and alternatives

Baseline: current interpreter entry in runtime/src/function.cpp, after argument
validation and overload selection. Keep the same public ABI and result writer.
Use fixed-shape scalar/vector/matrix functions, runtime early exits, multiple
shapes and overloads, integer arguments/results, print/reject, and unsupported
recursive/dynamic-shape cases. Verify result type, dimensions, storage order,
error recovery, and zero interpreter entries on admitted repeated calls. Keep
independent CmdStan function expressions exercised through small models where
possible. Measure handle creation, first call, repeated calls and memory; a
cached call must amortize preparation on ordinary small functions.

Alternatives: (1) compile through the existing shared register compiler and
cache a bounded number of immutable programs; (2) lower each function into a
model graph with synthetic input/output declarations; (3) introduce general
runtime call frames and dynamic storage now. Start with (1): (2) adds a second
adapter and synthetic model semantics, and (3) is a larger language-runtime
change unnecessary for proving useful coverage. Neither alternative requires
machine-code generation.

## Proof and scope

Validate arguments and resolve overloads on every call, before cache lookup.
Key a program by the selected definition, full logical extents, and any integer
values specialized by compilation. Runtime real values must not enter the key
or be folded. Bound the cache; use immutable shared programs and per-call value
buffers so concurrent calls cannot share execution state. Cache refusal too.
Never retry an execution error through the interpreter: print/reject and other
effects may already have happened. Only a pre-execution compile refusal can
select the established fallback.

Begin with scalars, vectors, row vectors, matrices and one-dimensional scalar
arrays, where API and register storage orders agree. Multidimensional arrays
need an explicit first-index-fast/API versus outer-major/register conversion.
Preserve return leaf type using typed return expressions; refuse missing or
inconsistent return metadata. Retain recursion/depth behavior and unsupported
shapes through the current interpreter until general runtime calls/storage have
an evaluator of their own. Do not silently narrow the public API.

This is a review checkpoint, not completion of the overall migration.

## Fable dispositions and first implementation boundary

[Review](2026-09-28-fable-standalone-function-review.md) confirms the direction
with required constraints. Leave all compiler evaluation/folding hooks empty,
including extern real/int/ints, higher-order lowering and target binding.
Integer specialization keys include every integer value, after promotion to the
selected formal type. Admission uses depth and leaf, never rank alone.
Execution tracing is thread-local and strict tests cover first compilation too.

Use an eight-entry FIFO of immutable plans/refusals, protected only for lookup
and insertion; execution owns its register vector. Cap retained register and
instruction counts across entries as well as entry count. First-slice output
admission is real-valued: integer results retain the interpreter until its
32-bit result mirrors and overflow behavior have an explicit shared contract.
The evaluator still exercises integer API results, boundary arithmetic and
recursion to prove the fallback preserves behavior. RNG calls retain the
existing unseeded-API behavior; this change does not add a seed or ABI.

Add changing extents/integer values, zero extents, array-of-vector rejection,
concurrent eviction, missing result metadata, errors/effects and recovery to
focused tests. Benchmark changing shapes as well as cache hits; do not hide
recompilation cost behind warm repeated calls. Report cache churn honestly.

## Measured cache-churn correction

The first candidate makes repeated sized-vector calls about ten times faster,
but cycling twelve signatures through an eight-entry FIFO recompiles every call
and regresses roughly 12.5 → 17 µs. Do not ship that replacement policy.

Bounded next experiment: after the cache fills, require the same missing
signature on two consecutive cache misses before compiling/evicting. A lone new
shape uses the established interpreter without speculative compilation; existing
cache hits remain compiled, and a stable new shape promotes on its next miss.
This is a generic cache-admission policy, not shape/model special casing.
Retain the initial candidate measurements and remeasure identical workloads.
Accept only if repeated-call gains remain and the rotating-signature workload
beats the interpreter baseline. This adds an explicit temporary interpreter
boundary; eventual interpreter deletion still needs reusable dynamic storage or
another generic value engine for unbounded signature churn.


## Final semantic guardrails

The C++ DataMap scalar-integer setter accepts long and can retain a real mirror
that differs from its narrowed Stan int. Such inputs select the interpreter
before cache lookup, so they cannot collide with canonical integer inputs.
Integer computations (including integer-valued subexpressions promoted into a
real result) remain on the established engine for this first slice. Structural
queries/predicates and checked literal negation are admitted; arithmetic and
integer reductions require a later explicit 32-bit contract. The scan follows
called functions too.

The public API now clears stale integer mirrors on proven typed real results
from either engine, matching the declared real surface rather than letting a
cache admission decision change result type. Genuine integer outputs retain
both mirrors and the interpreter. Tests cover promoted real returns and
noncanonical integer inputs in addition to ordinary scalar/vector/matrix cases.

A constructor domain error during speculative compilation becomes a refusal
(resource exhaustion still propagates). It is reported only if execution takes
the offending branch. A failing linspaced_vector(-1, ...) regression proved the
issue in both standalone compilation and the shared ODE/DAE adapters before
those catches were corrected. Print/reject effects remain ordered and are never
re-executed through a fallback after compiled execution has begun.
