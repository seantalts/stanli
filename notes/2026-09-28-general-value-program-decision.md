# Deferred proposal: a simpler program for remaining interpreter work

**Status: deferred.** The goal is to prevent surprising performance cliffs,
not to remove `MirInterp` for its own sake. First extend the existing graph,
register and structured-loop engines where they can handle current fallbacks.
A replacement is worth considering only if measurements show a useful speed
benefit for the remaining cases. The prototype below is a possible later
experiment, not the next implementation step.

**Earlier proposal: build a small, optional prototype for the cases that still
need general-purpose execution.** It would turn the model's code into a compact
list of instructions before running it. Instructions would refer to numbered
value slots and known functions, instead of repeatedly walking the compiler's
tree representation and looking up variables by name.

Keep the existing graph and register engines for the work they already handle
well. The experiment should establish whether this approach can replace the
remaining uses of `MirInterp` without losing Stan features or making ordinary
models slower. It is not a commitment to replace the whole runtime.

Native performance is the priority. Machine-code generation and stencil JIT
research remain tabled. This proposal needs neither of them, and includes no
Wasm work. No prototype has been implemented yet.

## How models run today

The compiler first translates Stan source into **MIR**, an internal description
of the model's expressions, statements and functions. Think of it as a tree:
an assignment contains an expression, which may contain a function call, which
in turn contains its arguments.

Stanli can execute work from that description in several ways:

| Mechanism | What it does | Why it matters here |
| --- | --- | --- |
| Graph executor | Prepares operations and their input/output buffers, then calls the selected numerical routines. | Much of the main model calculation already runs this way. One operation can do substantial vector or matrix work. |
| Register program (`Program`) | Runs a list of small instructions over numbered memory slots, including arithmetic, branches and calls to numerical routines. | Handles many scalar calculations, runtime decisions and solver callbacks efficiently. Its “registers” are memory slots, not CPU registers. |
| Structured loop engine | Keeps supported loops as reusable execution plans, with the history needed to compute derivatives. | Avoids expanding every loop iteration into a separate prepared operation. |
| MIR interpreter (`MirInterp`) | Walks the MIR tree as it executes, looks up values and manages function calls and containers. | Handles general cases that do not fit the prepared engines, as well as work during model preparation and initialization. |

These mechanisms can call one another. For example, the graph can invoke a
solver, and the solver can repeatedly call a user-written Stan function through
either a register program or the MIR interpreter. A **callback** is that
user-written function supplied to another routine.

All of these engines are implemented in compiled C++. A **kernel** is a
compiled numerical routine they call, such as a matrix operation or probability
calculation. Kernels are shared building blocks, rather than another competing
way to execute an entire model.

**Instruction dispatch** means reading the next operation and selecting the
code that performs it. The register engine dispatches instructions; the graph
walks prepared calls to selected routines. Both avoid much of the MIR
interpreter's tree traversal and name lookup, but still dispatch operations.

The recent work has moved more function returns, standalone function calls and
solver callbacks onto existing compiled paths. It has also reduced derivative
recording costs in selected kernels. There are more improvements to make in
those engines independently of this proposal.

## What still needs a general execution path

The key distinction is between **changing values** and **changing sizes**.
A vector can contain different numbers on every model evaluation while keeping
the same length. Existing prepared buffers handle that already.

Some supported functions need more flexibility. For example:

- A function accepts an integer `n` and creates a local vector of length `n`.
- A function returns a container whose length differs between calls.
- A recursive function calls itself before its previous invocation has
  finished. Each invocation needs its own arguments, local values and return
  destination, subject to the existing recursion-depth limit.

These are existing requirements, exercised by the standalone-function tests
`dynamic_result`, `sized` and `descend`. They are not new language features
proposed here.

A register `Range` describes a prepared section of storage with a fixed length.
Caching one prepared program for each set of argument sizes helps when those
sizes repeat. It does not, by itself, provide storage for every possible new
size, changing local allocation or recursive call. A bounded cache still needs
a correct way to handle cases outside its prepared entries.

Other remaining uses of the MIR interpreter include:

- **Preparation:** evaluating transformed data and trying expressions whose
  inputs may be only partly known, to decide what can be prepared in advance.
- **Initialization:** converting supplied initial values into the model's
  internal parameter representation.
- **Outputs:** executing output blocks whose runtime behavior does not fit the
  prepared paths.
- **Some solver callbacks:** handling runtime-sized local containers or runtime
  integer/control arguments that the current callback compiler cannot accept.

There are also narrower gaps we can fix within existing engines. For example,
nested callback arrays need explicit conversions between storage layouts.
That issue alone does not require a new execution engine.

To delete `MirInterp` while keeping current language and API support, something
else must execute all these general cases. Expanding fixed-size coverage is
useful, but does not remove that requirement.

## The proposed replacement

The proposed **typed-value program** is a list of instructions whose numbered
slots can hold Stan values: integers, real numbers, vectors, matrices and
arrays. “Typed” means the program knows what kind of value each operation
expects. A container's size can be determined when the instruction runs.

For illustration, a function that creates and returns a vector might become
instructions like these. This is explanatory pseudocode, not a proposed file
format:

```text
read argument n into slot 0
allocate a vector of length slot 0 into slot 1
fill slot 1 using the existing numerical operation
return slot 1
```

The compiler would resolve variable names and function targets once. Execution
would then use the numbered slots and known targets directly. A branch would
select the next instruction; a function call would create a **call frame**:
the arguments, local values and return location for that invocation. Recursion
would create additional frames, preserving the caller's values until return.

The possible benefit is less repeated tree traversal, name lookup and setup.
How much this saves is a measurement question. Container allocation and the
underlying numerical work still have costs.

The program must reuse existing implementations of Stan operations and their
checks. It must preserve shapes, errors, random-number state and observable
effects such as printing or rejection. This is a change in how operations are
organized and executed, not a project to rewrite Stan's numerical formulas.

### Does this actually remove the interpreter?

It could eventually remove **the current MIR tree interpreter**. In the broader
sense, the proposed engine would still be an interpreter: a compiled C++ loop
would read instructions and execute them. The graph and register engines also
retain dispatch.

This proposal therefore targets removal of `MirInterp` and its repeated
execution overhead. It does not promise a model with no instruction dispatch.
It needs no per-model C++ compiler, JIT or executable-memory allocation. Users
would still install a self-contained runtime.

### What happens to derivatives and taped kernels?

Stan needs derivatives of the model's log density for sampling. Some kernels
compute these directly; others use Stan Math's automatic differentiation. That
system records operations and the state needed to propagate derivatives
backward. This record is called a **tape**.

A kernel can use a tape regardless of which execution engine called it.
Replacing `MirInterp` therefore does not automatically remove taped kernels.
Improving those kernels remains a separate performance task.

The first prototype would compute ordinary numeric values using `double`,
without derivatives. Its design must also allow the same execution logic to
use Stan's existing reverse-mode values (`var`) and tape when derivatives are
needed later. We would not introduce a new derivative-recording system. Solver
callbacks that need derivatives would require their own correctness and
performance checks before migration.

## Why use a separate program?

There are three reasonable choices:

| Choice | Benefit | Cost or limitation |
| --- | --- | --- |
| Extend the existing register `Program` | Keeps more execution in one program format. | Requires adding changing-size values, allocation and call frames alongside the current flat buffers and derivative machinery. That makes the existing engine more complex. A slowdown is a risk to measure, not an inevitable result. |
| Add a separate typed-value program | Gives general values and function calls an explicit home while leaving the fast flat instructions alone. | Adds another internal representation to maintain. It must share operation implementations with the existing engines to avoid duplicating semantics. |
| Keep `MirInterp` indefinitely | Requires the least architectural change and allows other performance work to continue. | Leaves its remaining overhead and does not meet the eventual removal goal. |

The earlier recommendation was to test the second choice: its separation
would make an experiment easier to evaluate without changing the storage rules
of hot register instructions. That experiment is now deferred while existing
engines can remove measured performance cliffs.

This need not become a permanently additional engine. We could implement it
alongside `MirInterp`, or gradually rewrite `MirInterp` to resolve names in
advance and use explicit frames. Those approaches could reach the same final
representation. The prototype should help determine the migration route.

## A small first experiment

Keep production defaults unchanged and make the prototype optional. Start
with the existing standalone-function API, where the requirements are concrete
and easy to exercise independently of a full model.

Support only enough operations to demonstrate:

1. A local container whose size is chosen during execution.
2. A return value whose size changes between calls.
3. A recursive helper with separate local values for each call.

This requires basic arithmetic, existing container operations, branches, calls
and returns. Reuse the current semantic helpers. Do not add new value-type or
size checks to the existing hot flat instructions.

### What would count as success?

**Correct behavior comes first.** Compare with both `MirInterp` and recorded
independent CmdStan references. Check values, integer and overflow behavior,
container dimensions, effects, rejection and recovery after an error. Require
exact agreement where the operation order is identical; retain the project's
numerical checks elsewhere rather than relaxing tolerances for the prototype.
Repeat calls with changing sizes, including after failures.

**Measure the costs separately.** Record preparation time, the first call,
repeated calls with the same sizes, calls alternating among different sizes,
peak memory, memory retained between calls and compressed library size.
Existing graph/register workloads must remain unchanged, and models that do
not use the prototype must do no extra preparation work.

**Continue only with useful evidence.** All three function requirements must
work, and an existing interpreter workload must show a first-call or
one-off-call benefit larger than normal timing variation. Measure that
variation by comparing repeated runs of the unchanged implementation. Report
memory and package-size costs alongside any speedup.

If the first version is slower, further work needs a specific measured cost
we believe we can remove and a bounded follow-up experiment. Changing the
reported execution-engine name is not a performance result. The recently
compiled fixed-shape generated-quantities callbacks should serve as regression
checks, not as evidence of a speedup for this new engine. Remaining callbacks
with runtime integer/control arguments may provide later real-model workloads.

### Details the prototype must get right

**Separate shared code from each call's storage.** Prepared instructions may be
shared, but each invocation needs its own frames and value buffers. Test
simultaneous calls, removal of cached programs, recovery after exceptions and
how long referenced storage remains valid.

**Define container layout and ownership.** The existing `Range` and `DataMap`
representations agree on plain matrix layout, but not on every nested-container
layout. State how values are stored, who owns their memory and how long views
remain valid. Avoid copies only where compatibility is established; measure
necessary conversions and copies.

**Avoid making preparation more expensive.** During preparation, Stanli tries
to evaluate expressions using the values already known. Compiling a fresh
program for every tiny attempt could cost more than it saves. Any later
migration of these evaluations needs reusable prepared state. If an attempt
encounters an unknown input, it must preserve the existing rules for stopping
without leaving unintended changes or effects behind.

**Keep execution reporting honest.** Mark opt-in executions as
`typed_value_program` and add tests for that route. Keep the existing tests
that detect interpreter use until each production route is deliberately
migrated and verified.

## What would remain before deleting MirInterp?

A successful prototype would answer the representation question, not complete
the migration. Later work would still need to cover:

- All required built-in functions and container operations.
- Random-number generation and observable effects.
- Initial-value conversion and dynamic output blocks.
- Preparation with only partially known inputs.
- Solver callbacks that require automatic differentiation.

Each is a separate correctness and performance milestone. Keep `MirInterp`
available internally as a comparison implementation during migration, and use
independent CmdStan checks as well. Remove it from production only when every
required entry point has a verified replacement.

If measured cliffs remain after extending the existing engines, this experiment
could compare a separate general-value program with adding those capabilities
to `Program`. Neither deserves adoption merely for replacing `MirInterp`.
The current work is [closing gaps in the existing engines](2026-09-28-runtime-for-coverage.md).

## Background

The [execution-engine roadmap](2026-09-28-execution-engines-and-roadmap.md)
contains the broader inventory and implementation history. The
[Fable architecture review](2026-09-28-fable-architecture-review.md) records the
review behind this proposal and the resulting requirements. This rewrite
explains that reviewed proposal; it adds no implementation.
