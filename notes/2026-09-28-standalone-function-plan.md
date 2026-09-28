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
