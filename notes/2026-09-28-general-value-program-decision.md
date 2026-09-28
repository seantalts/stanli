# Design decision: representing the remaining dynamic Stan values

This is an architectural choice, not a request to resume instruction-generation
research or a choice between native performance and browser work. Native
performance remains primary. Existing graph/register/kernel improvements can
continue independently; their existence does not decide this representation.

The completed native work includes fixed-shape function exits, bounded cached
standalone register plans, two measured tape-removal changes, shape-preserving
matrix callbacks, and the value-only callback work recorded in this turn.
These do not establish that every fast-path opportunity has been exhausted.
They do establish that extending admission into existing fixed ranges cannot
by itself cover every required interpreter behavior.

## Concrete surviving requirements

The standalone function API already supports changing output extents,
recursion within its depth guard, and runtime-sized local containers (the
existing dynamic_result/descend/sized tests). Preparation also evaluates
transformed data and partial environments during folding/admission; initial
value transforms and genuinely dynamic output blocks retain MIR interpretation.
Modern retained callbacks with runtime-sized locals or runtime integer/control
arguments remain visible fallbacks. Nested callback arrays additionally need
explicit reversible storage-order adapters; that smaller issue does not itself
require a new engine and is not being relabeled as an architectural blocker.

A fixed register Range has one prepared length and storage assignment. No
specialization cache of finite size removes the requirement to execute a new
or changing runtime geometry correctly. Deleting MirInterp therefore requires
an alternative general value execution representation, or a deliberate loss
of existing language/API support. Losing that support is outside the goal.

## Alternatives

1. Extend Program itself with runtime-shaped cells, allocation, call frames,
   and recursion. This keeps one instruction container, but its existing flat
   register and adjoint machinery would have to distinguish dynamic values
   and lifetimes. Careful separate opcodes could avoid a branch in every hot
   arithmetic instruction; a slowdown is a risk to measure, not a fact.
2. Introduce a separate compact typed-value program for these general cases,
   behind the same semantic operation registry. Keep admitted graph/register
   paths exactly as they are. Resolve names and callees once, execute numbered
   value slots and explicit call frames, and reuse the same builtins, shapes,
   effects, checks, RNG state and Stan numerical algorithms. This adds an
   internal representation and maintenance burden, but contains the new
   dynamic storage contract and gives the old MIR engine a staged replacement.
3. Retain MirInterp permanently for cold/dynamic cases while continuing native
   hot-path improvements. This is practical if complete removal is no longer a
   requirement, but does not satisfy the stated eventual deletion objective.

Recommendation: option2, subject to a bounded prototype before production
adoption. Whether implemented beside MirInterp or as a staged rewrite of it is
an experiment outcome; pre-resolving its names and making its frames explicit
can converge on the same representation. Permanent retention is the endpoint
where that migration stops, not a wholly different implementation fork. This is still instruction dispatch in compiled C++; it removes
recursive MIR-tree evaluation, not every meaning of the word interpreter.
It needs no JIT, per-model C++ compiler, executable-memory allocation, or Wasm
work. Package simplicity and small binaries remain gates.

## First reviewable experiment after the design choice

Keep the production default unchanged. Compile a tiny value-only surface into
numbered slots with explicit return/call/branch operations: scalar arithmetic,
existing shape/container primitives, one changing-size local, a recursive
helper, and one changing-size return. Exercise the existing standalone API
requirements first. Reuse semantic helpers rather than porting formulas into
new handwritten code. Do not add dynamic tags/checks to hot flat instructions.

Compare the prototype to MirInterp and pinned independent CmdStan references.
Require exact internal values/effects where operation order is identical,
integer overflow/mirror and shape fidelity, rejection and recovery behavior,
and repeated calls with changing geometry. Fix the initial AD contract now:
the value program is scalar-parameterized and reuses existing Stan var/tape
semantics when required. This project introduces no new dynamic adjoint stream.
The first experiment runs double; active retained callbacks are a later parity
and performance gate under that same declared contract. Measure preparation, first and warm
calls, rotating shapes, retained/peak bytes and compressed library growth.
Require unchanged graph/register canaries. Immutable plans may be shared,
but frame/value buffers are per invocation, with concurrency, cache eviction,
exception recovery and lifetimes tested from the first prototype.

Measure preparation probes on ordinary models separately; compiling a tiny
expression per folding probe is not assumed profitable. Models that do not
use this prototype must incur no additional preparation work. A later probe
migration needs reusable decoded contexts and the existing transactional
unknown-value/effect contract.

Do not assume Range and DataMap share every layout: plain matrices agree,
but nested containers currently need storage adapters. Define one explicit
value-layout/ownership contract, use zero-copy handoff only for proven compatible
views, and measure conversion/copy costs and alias lifetimes for the others.

Use an opt-in `typed_value_program` execution label. Keep existing default
interpreter-event tests until each route is deliberately migrated; add
prototype-specific assertions rather than deleting those canaries. Continue
past the first experiment only with all three dynamic function requirements
correct and a measured first-call or one-off-signature benefit beyond A/A
variation on an existing fallback workload. Report memory and package-size
costs together with speed. The newly compiled fixed-shape GQ callback route
is a canary, not a suitable target to claim a general-runtime speedup. Runtime
integer/control GQ fixtures remain potential later real-model fallbacks. A slow initial stage needs a bounded
continuation case with measured removable costs; it must not become a new
fallback solely because it changes a diagnostic label.

Only after that experiment should the design cover full builtin/container
coverage, RNG/effects, constrained initialization, partial-environment probes,
and retained autodiff callbacks. Those are separate completion gates. Keep
MirInterp as an internal differential reference during migration; delete it
from production only when every mandatory entry point is independently covered.

No prototype implementation is included in this proposal. Choosing the general
value representation changes compiler/runtime architecture and long-term
maintenance, so it is the substantive design checkpoint requested by the user.
