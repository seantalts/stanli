# Next coverage slice: shape-preserving retained callbacks

Current work continues under the native-performance priority. Native machine
instruction generation and stencil JIT remain tabled. This is a shared ABI
within existing graph/register/interpreter engines, not a new execution engine.
Implement after the separately measured Cholesky-correlation change is saved.

## Source evidence

RhsArg currently stores only integer/data/active classification and flattened
length. compile_rhs_args/compile_dae_args therefore reject matrix and nested
array formals in supported_rhs_view. stamp_rhs_view recovers only one-dimensional
kind. The interpreter fallback's positional call overload likewise invents a
single dimension from flattened length. Consequently simply lifting compiler
admission would not establish a correct fallback or preserve matrix geometry.

The shape information exists earlier: graph LoweredArgument has Val/SlotInfo
and cached typed DataMap::Entry; register lowering has Range logical views;
higher_order_eval has typed values before storage_order flattens them. Modern
ODE graph lowering has its own packing loop; program and quadrature lowering
share pack_callback_arguments. DAE, adjoint ODE, algebra and quadrature each
reconstruct the flattened argument lists in their kernel adapters. Every
affected path must agree, rather than fixing only one callback entry.

## Bounded first implementation

Start with fixed-shape matrix arguments, including non-square and zero extents.
Preserve column-major values and the complete two-dimensional shape in RhsArg
at preparation. Keep scalar/one-dimensional legacy bindings compatible. Use
one shared view-validation/binding helper for compiled callbacks, and one
shared typed-value reconstruction helper for interpreted fallback. The existing
typed MirInterp::call overload preserves complete dimensions and is preferable
to teaching each solver a separate shape convention.

All dimensions are immutable preparation-time metadata; real matrix entries
remain runtime values. Check rank, checked extent product, flattened length,
formal leaf and array rank before admission. Keep the ordinary compiled hot
callback's register seeding unchanged. Preserve y/theta ordering, active/data
classification, effects and result-shape checks. Do not extend the exact direct
RK whitelist as part of this slice.

Do not enable nested arrays merely because matrices pass. Graph/register arrays
are outer-major while interpreter arrays are first-index-fast. A subsequent
array extension needs explicit reversible storage adapters for both real and
integer arrays, including arrays of vectors/matrices. Keep that boundary
visible and refuse unsupported views rather than guessing dimensions.

## Evaluator before implementation

First reproduce the current matrix callback refusal and fallback behavior with
a tiny linear ODE, using both data and active non-square matrix arguments and
row/column indexing that distinguishes a flattened vector from a matrix.
Then compare compiled callback values and weighted y/theta derivatives bitwise
against shape-preserving typed interpretation. Verify changed matrix values
at the same shape cannot become stale constants. Exercise nested UDFs, early
returns, zero extents, invalid rank/extent products and unchanged legacy calls.

Force a genuine compiler refusal inside a matrix callback and verify the
interpreted adapter still preserves dimensions and effects exactly once. Add
solver-level independent pinned CmdStan references for the admitted matrix
forms and representative DAE/quadrature paths if their signatures support them.
Exercise graph, retained register-region and value-only higher-order callers;
do not infer cross-context coverage from one adapter.

The historical matrix path may not execute correctly, so it cannot provide a
valid timing baseline. If confirmed, first establish a separate shape-preserving
interpreted baseline, then compare callback and complete-solve native phases.
Use scalar/vector callback canaries to detect added preparation or runtime
cost. Record metadata memory and binary growth; clean rebuild affected C++
consumers, full native tests, complete recorded corpus and installed interfaces.

This is the next planned coverage experiment, not an implemented or validated
matrix-support claim. General runtime-sized results, arbitrary recursion and
call frames remain a later value-runtime architecture project.

## Fable review adjustments

Range already carries matrix rows/columns and supports matrix-local indexing;
RhsArg transport, supported_rhs_view and stamp_rhs_view are the missing pieces.
Use optional dimensions with missing matrix geometry refused explicitly. Verify
the current fallback with the new non-square active/data fixture before runtime
edits. Pin CmdStan as the independent primary model oracle; internal typed
interpretation is a consistency oracle, not independent correctness evidence.
Also compare direct RK enabled/disabled, since indexed scalar matrix arithmetic
may already satisfy its unchanged whitelist. Add the equivalent flattened-vector
callback as a supported user-facing baseline. Keep legacy scalar/vector bitwise
canaries when moving any fallback onto typed invocation. See the adjacent Fable
review for exact findings and scope limits.

## Implemented representation and review follow-up

The optional geometry is represented by two inline int64 extents with -1 meaning
absent. Zero is valid. This adds 16 bytes per RhsArg and no per-argument heap
allocation; compiled hot register seeding is unchanged. Both compilers validate
matrix metadata before storage allocation and stamp the existing Range view.

Only callbacks containing a plain matrix use typed fallback reconstruction.
Scalar/vector-only callbacks retain the old positional invocation. Inside a
matrix callback, typed invocation additionally records declared formal extents;
this is intentional and checked against independent model results. Arrays of
matrices and nested arrays remain outside this admission contract.

Fable's implementation review identified missing end-to-end tests for empty
JSON matrices and expression-valued matrices. The follow-up fixture passes a
data transpose before an integer array and an active transpose/product, plus
0x3 and 3x0 JSON matrices, through compiled and deliberately refused callbacks
and value-only generated quantities. Matrix predicates now use UnsizedView
consistently, and a missing matrix rank produces a named preparation error.
The review could not read the large lowering source within its tool budget;
the lead inspected all packing sites and the model engine-selection test checks
them through the public diagnostic output.

The combined context fixture intentionally retains the pre-existing whole-GQ
fallback: a real argument marked data-only by stanc can still be a runtime
value in generated quantities. The current retained-call packers demand it as
a preparation constant. This is a distinct classification gap, not a missing
matrix view; changing it requires preserving the distinction between runtime
availability, autodiff activity, and formally data-qualified arguments. The
matrix patch tests that existing fallback's value-only packer explicitly.
