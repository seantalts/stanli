# How stanli works and why it is fast

## Abstract

HMC and NUTS evaluate the same log density and gradient many times; only the
parameter values change. stanli exploits this by preparing a reusable
execution plan once, after reading the Stan program and its data.

stanli runs the official `stanc` front end, then lowers the optimized program
to a fixed graph of precompiled C++ operations. At model load it assigns fixed
locations to graph values, graph adjoints (reverse-mode derivative
accumulators), and each operation's declared scratch space. A native-path
gradient then loads new parameters and runs the outer forward and reverse
sweeps over double buffers.

Four mechanisms provide most of the speedup:

1. **No model-specific C++ build.** stanli does not compile and link generated
   C++, so model preparation is much shorter.
2. **A reusable reverse-mode plan.** The graph and most derivative storage are
   built once, instead of reconstructing a model-wide Stan Math autodiff tape
   for every gradient.
3. **Vector-sized operations.** One graph operation can evaluate a vector
   expression or density over all observations, so interpreter dispatch is
   paid per vector operation, not per scalar element.
4. **Specialization using the loaded data.** Data-only expressions are
   computed once. Known loop bounds, shapes, indexes, and data-only branches
   let stanli recover broadcasts, slices, gathers, and independent batches
   from scalar loops.

Models with large vector operations, or scalar loops Stanli can turn back into
vector operations, gain the most. Models dominated by one large Stan Math
kernel have less interpreter overhead to remove. The [full corpus benchmark](benchmarks.md)
reports the measured effects with numerical checks and sampling diagnostics.

## Scale of the four effects

These are rough scales for intuition. The effects overlap, and actual gains
depend on model structure.

| mechanism | rough scale | why it helps |
| --- | --- | --- |
| Skip model-specific C++ compilation | Model setup takes milliseconds rather than seconds on small models; the first complete Eight Schools run is roughly 100x faster. | stanli builds a graph in memory instead of invoking a C++ compiler and linker. |
| Reuse the reverse-mode plan | Around 2x when rebuilding the autodiff tape is a substantial part of gradient cost. | Fixed graph and derivative storage replace repeated tape construction. |
| Execute vector-sized operations | Commonly a few times faster on vector-heavy models; sometimes close to 10x. | One dispatch can process tens, hundreds, or thousands of elements. |
| Specialize and recover loops using data | Several-fold on suitable loop-heavy models. | Data-only work is done once, and scalar iterations can become a few vector batches. |

Constant folding is the simplest form of data specialization: an expression
that depends only on data cannot change during sampling, so stanli evaluates
it once at model load.

Loaded data can also determine graph structure. If `group[n]` is data and
selects one of two likelihoods inside a loop, every branch outcome is fixed
before sampling starts. stanli can then split the observations into two
independent sets and run one vectorized likelihood per set — a scalar loop
with an `if` becomes two vector batches, without changing the model. A worked
example appears in [Partitioning loops using data](#partitioning-loops-using-data).

## The work repeated during sampling

At an HMC position `q`, each leapfrog step needs `log p(q)` and its gradient.
Parameters change between calls; the data, declared shapes, and most of the
computation do not. The useful distinction is work done once per model versus
work done once per gradient.

CmdStan has two stages:

1. `stanc` translates the model to C++, then a C++ compiler builds and links
   it.
2. The compiled model evaluates with Stan Math reverse-mode autodiff. Each
   gradient call creates the autodiff inputs, records the work for that call,
   runs the reverse pass, copies the gradient, and recovers the autodiff
   arena. The benchmark driver in
   [`tools/bench_cmdstan_grad.cpp`](../tools/bench_cmdstan_grad.cpp) shows this
   sequence directly.

stanli still uses `stanc` for parsing, type checking, and Stan-specific
optimization. It replaces the model-specific C++ build and the repeated
model-wide autodiff recording with a reusable graph.

Note that native compilation and autodiff recording are separate costs.
Compiling a model fixes its machine-code instruction sequence; Stan Math still
constructs the autodiff state each time that compiled function runs.

## From Stan source to a bound operation graph

Consider this statement:

```stan
y ~ normal(mu + tau * theta_tilde, sigma);
```

CmdStan's generated C++ arranges arithmetic and probability functions from
Stan Math. The model-specific information is mostly which functions run, in
what order, with what shapes and connections. stanli stores that arrangement
as data:

```text
MUL          tau, theta_tilde -> scaled       # vector scale
ADD          mu, scaled       -> theta        # scalar broadcast
NORMAL_LPDF  y, theta, sigma  -> target       # summed vector density
```

Each row is an operation, or *op*, containing an opcode and indexes into fixed
buffers. The C++ implementation of each opcode is already compiled into the
stanli shared library.

Model preparation has four stages:

```text
Stan source + data
        |
        v
stanc: parse, type check, optimize
        |
        v
lowering: compiler representation -> operation graph
        |
        v
graph passes: fold constants, recover loops, remove redundant work
        |
        v
binding: allocate buffers and resolve kernel pointers
```

This is still compilation in the broad sense — the model is analyzed and
converted to a lower-level representation — but no model-specific native C++
is compiled or linked.

### Storage is assigned once

Binding creates three main arrays:

- The **value arena** holds parameters, data views, intermediate values, and
  the log density.
- The **adjoint arena** holds derivatives for parameters and live
  intermediates. Data and slots removed by graph optimization need no adjoint
  storage.
- The **scratch arena** holds partial derivatives and other values needed by
  reverse kernels.

Here, *arena* means a contiguous array whose offsets are chosen at model load
— not Stan Math's dynamic autodiff memory pool, discussed in the next section.

Binding also gives each op direct input, output, adjoint, and scratch
pointers, and resolves the forward and reverse functions into two flat lists.
The setup code is in
[`runtime/src/executor.cpp`](../runtime/src/executor.cpp).

A gradient evaluation is then approximately:

1. Copy the unconstrained parameters into their assigned cells.
2. Run the forward list, computing values and saved partials.
3. Clear the compact adjoint arena and seed the log density with adjoint 1.
4. Run the reverse list, accumulating adjoints.
5. Copy the parameter adjoints to the caller's gradient vector.

Names, shapes, offsets, pointers, and kernel lookup are not rebuilt, and the
executor allocates no memory in this loop. Some generic kernels and fallback
paths may still allocate temporary storage; those cases are described below.

## Why a memory pool does not make autodiff allocation free

Reverse-mode autodiff first computes values and saves the local information
needed for derivatives, then seeds the result with derivative 1 and applies
the chain rule in reverse order. The saved record is usually called the
autodiff tape.

Stan Math does not normally call the general-purpose heap allocator for every
scalar operation. A `var` refers to a reverse-mode object (a `vari`) in an
arena that obtains memory in blocks and hands it out cheaply. After a gradient
evaluation, `recover_memory()` resets the arena in bulk instead of freeing
each object separately.

This makes obtaining and reclaiming bytes cheap, but it does not preserve a
constructed tape. Each gradient evaluation must still:

- create and initialize records or callbacks;
- store operand references and partial derivatives;
- register the reverse work;
- write and later read the records, using cache and memory bandwidth;
- traverse the registered work in reverse; and
- reset the tape for reuse.

The pool reuses memory, but the runtime reconstructs and touches its contents
at the next leapfrog step.

Modern Stan Math reduces this cost substantially: a vectorized function can
use one callback with arrays of partials, and `var_value` can represent a whole
vector or matrix with one value/adjoint object. The record count therefore
depends on the expression and the available overload. The comparison here is
about repeated construction and metadata, not a claim that Stan Math always
uses one tape object per observation.

The two execution styles compare as follows:

| stage | CmdStan with dynamic Stan Math reverse mode | stanli native graph path |
| --- | --- | --- |
| Before a call | Arena blocks and tape-vector capacity may be available for reuse. | Graph, contexts, and value, adjoint, and scratch buffers are already bound. |
| Forward | Compute values while creating autodiff records or callback state. | Write values and partials to predetermined offsets. |
| Reverse | Traverse the work registered during this evaluation and follow its operand references. | Walk a fixed reverse-op list over contiguous adjoint buffers. |
| Cleanup | Rewind the tape and arena; the retained memory is empty again. | Keep the graph and storage; clear only the required adjoints before the next call. |

### A concrete vector example

For a vector of length `N`, consider:

```stan
theta = mu + tau * theta_tilde;
y ~ normal(theta, sigma);
```

With operator-overloaded reverse mode, the arithmetic creates autodiff
intermediates, and the density records the operands and partials its reverse
implementation needs. Vectorized Stan Math overloads make this much cheaper
than a scalar tape, but the state is still produced during each evaluation and
discarded when the arena is recovered.

In stanli, `scaled` and `theta` are length-`N` double buffers at fixed offsets.
`MUL` and `ADD` each loop once over those buffers. The density writes its value
and saved partials to assigned scratch space, and reverse kernels loop over
the same arrays to update fixed adjoint buffers. The graph performs three
dispatches, not `N` copies of three graph operations.

Several kernel paths sit behind this interface. Analytic kernels operate
directly on double buffers, and many save the values needed by the reverse
pass in fixed scratch. Generic probability kernels use a small recording
scalar so the existing Stan Math template computes its usual partial
derivatives; for a vector input that depends on parameters, this path
currently creates a temporary Eigen array of partials before copying it to
scratch, while specialized hot kernels avoid that temporary. See
[`runtime/include/stanli/recorder.hpp`](../runtime/include/stanli/recorder.hpp).

Functions without an analytic or recorder path build a nested Stan Math tape
inside that kernel. This preserves coverage and reference behavior but retains
dynamic autodiff cost locally. See
[`runtime/include/stanli/legacy.hpp`](../runtime/include/stanli/legacy.hpp).
So preallocation applies to executor-owned arenas and declared op scratch, not
to every temporary in every supported kernel.

## Why operation-level interpretation is cheap enough

Interpretation adds dispatch overhead; what matters is how much work each
dispatch performs.

The microbenchmark in
[`tools/bench_opcost.cpp`](../tools/bench_opcost.cpp) measures roughly 17--20
ns for one scalar density op's complete forward and backward execution,
including executor and recorder overhead. The density arithmetic alone takes
about 0.9 ns on that machine. A graph with thousands of scalar ops will
therefore be slow.

A vector op changes the ratio. One `NORMAL_LPDF` op can process 1,000
observations in compiled C++ while paying graph dispatch once. Its kernels
loop over contiguous values and partials with no graph metadata per element,
and contiguous storage reduces pointer chasing and improves cache behavior.
Eigen or the compiler may vectorize some kernels, but stanli does not require
SIMD: some kernels preserve a scalar reduction order for numerical agreement
with Stan Math.

CmdStan has no interpreter dispatch, but its reverse-mode path still builds
and traverses the autodiff state the expression requires. For models with many
medium or large vector operations, that cost can exceed stanli's
operation-level dispatch cost. If one large Cholesky factorization or matrix
multiplication dominates a model, both runtimes spend most of their time in
the same numerical kernel, so performance is usually similar.

## Recovering vector operations from scalar loops

Explicit loops are common in Stan and often clearer than manually vectorized
code. When loop bounds and indexes are known from the data, stanli first
expands the loop during lowering. For example:

```stan
for (n in 1:N)
  y[n] ~ normal(alpha[county[n]], sigma);
```

This initially produces repeated scalar lanes:

```text
INDEX        alpha, county[1] -> a1
NORMAL_LPDF  y[1], a1, sigma  -> lp1

INDEX        alpha, county[2] -> a2
NORMAL_LPDF  y[2], a2, sigma  -> lp2

...
```

Left in this form, the graph would pay interpreter overhead for each
observation. The re-roll pass in
[`runtime/src/reroll.cpp`](../runtime/src/reroll.cpp) finds repeated patterns,
proves their lanes independent, and classifies their inputs:

- `sigma` refers to the same slot in every lane, so it remains scalar and
  broadcasts.
- `y[n]` is parameter-free. Constant folding removes the scalar reads, and
  re-rolling collects the values into one load-time vector.
- `alpha[county[n]]` uses data-known indexes, so its scalar reads become one
  gather. Repeated county indexes are valid; the gather's reverse pass sums
  their contributions into the corresponding `alpha` elements.
- Values produced inside each lane become vector values consumed by the next
  vector op.

The rewritten graph is approximately:

```text
GATHER       alpha, county[1:N] -> county_alpha
NORMAL_LPDF  y, county_alpha, sigma -> target
```

This has the same vector granularity as a hand-vectorized Stan statement.

Re-rolling also recognizes slices, arbitrary gathers, repeated element
writes, values shared by all lanes, and distinct data constants across lanes.
In mixtures, it keeps per-observation log densities separate through
`log_mix` or `log_sum_exp` and reduces only at the end; it does not sum
early, which would change the model.

The pass declines a rewrite if one lane reads a parameter-dependent result
from the previous lane, an intermediate escapes the region in an unsupported
way, or an operation is unsupported or effectful. A recurrence is sequential
work and must remain sequential.

### Partitioning loops using data

Some independent lanes are not adjacent because data selects different work
for each observation. For example:

```stan
for (n in 1:N) {
  if (group[n] == 1)
    y[n] ~ normal(mu_1, sigma_1);
  else
    y[n] ~ student_t(nu, mu_2, sigma_2);
}
```

If `group` is data, every branch is known at model load. When the rewrite is
safe and profitable, stanli groups observations that took the same computation
shape and vectorizes each group separately. In this example, an interleaved
sequence of scalar branches becomes one vectorized normal likelihood and one
vectorized Student-t likelihood.

This depends on the condition being data-only. If the branch depends on a
parameter, its outcome can change at every gradient evaluation and must
remain runtime control flow.

### The graph passes that support loop recovery

Several passes expose and preserve vector structure:

| pass | purpose |
| --- | --- |
| Safe in-place updates | Avoid copying a whole vector for assignments such as `v[n] = x` when no later forward or reverse work needs the old value. This prevents `N` element assignments from causing quadratic copying. |
| Store-to-load forwarding | Remove a write followed immediately by a read of the same element, exposing the arithmetic lane underneath. |
| Constant folding | Evaluate graph regions that depend only on loaded data. |
| Re-rolling | Combine adjacent, periodically repeated independent lanes. |
| Lane partitioning | Combine equivalent independent lanes that are interleaved or out of phase. |
| Common-subexpression elimination | Remove repeated work exposed by the structural rewrites. |
| Islands | Combine profitable scalar regions that cannot become vector operations. |

In-place cleanup repeats after re-rolling and partitioning to handle the
slice and strided stores those passes create. Full-extent store cleanup
removes writes that only rename a complete value. Islands run after the
vector passes, and target terms are reduced only after all these rewrites.

The passes and their safety conditions are documented in
[`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md); their exact
order is implemented in [`runtime/src/lower.cpp`](../runtime/src/lower.cpp).
Each listed pass has a diagnostic switch. The A/B harness compares the shipped
graph with selected passes disabled, and focused pass tests compare results
before and after transformation.

Two models show the scale of the structural changes:

- `radon_pooled` falls from 27,670 graph ops to 8. In the targeted re-roll A/B,
  it moved from 0.91x to 6.18x CmdStan; the September 11 snapshot records
  7.35x for the complete system.
- `election88_full` falls from 289,165 ops to 65. The September 11 snapshot
  records 4.16x CmdStan.

The targeted A/B isolates one optimization. The [archived corpus rows](../notes/performance/2026-09-11-posteriordb-benchmark.md#historical-posteriordb-model-results)
record those complete-system measurements. The [current benchmark](benchmarks.md)
measures the current build separately.

Re-rolling can change the association order of floating-point reductions, so
the optimized and scalar graphs may differ in their last few bits even though
the mathematical expression is unchanged. Differential and cross-path tests
check exact equality where expected and documented bounds otherwise; see
[`TESTING.md`](../TESTING.md).

## Scalar recurrences and generated reverse programs

Not every loop is independent. An HMM forward recursion, for example, computes
state at time `t` from state at time `t - 1`; evaluating all time steps as a
vector would change the calculation.

After the vector passes, stanli may combine a profitable scalar region into
one *island*: a compact instruction list over numbered registers. Register
locations and sizes are fixed at model load, so the forward pass runs on plain
doubles without a graph-context lookup per scalar instruction. A
generated-adjoint island keeps its forward values and checkpoints in
preallocated scratch; its backward pass uses a reusable thread-local adjoint
buffer that may resize on its first sufficiently large call. Replayed islands
instead use reusable thread-local register storage.

The main optimization is the island's reverse pass. An earlier implementation
replayed each island under `stan::math::var` for every gradient. That reduced
the outer graph's op count but built an inner autodiff tape, so most models
did not improve and several became slower. For an eligible branch-free island,
stanli now generates a second instruction list at model load that computes the
pullback directly on doubles. See
[`runtime/src/adjoint.cpp`](../runtime/src/adjoint.cpp).

This gives a direct comparison with dynamic tape construction. In the targeted
island A/B, `hmm_gaussian` falls from 42,926 graph ops to 11. Replaying the
island with a nested Stan Math tape measured 0.92x the graph baseline; the
generated reverse program measured 1.60x. Both modes used the same outer graph
and mathematical derivative, but replay reran the forward calculation while
building and traversing its nested tape, whereas the generated path ran only
the derivative program.

Islands still pay dispatch for each scalar instruction, so their likely gain
is smaller than for vector operations. A cost model leaves regions with too
little work or too much register traffic as ordinary graph ops.

Parameter-dependent control flow uses a runtime region whose branch sequence
may change between evaluations. The generated reverse pass does not apply
there, so that path uses nested-autodiff replay.

ODE right-hand sides accepted by the register compiler use a related program,
because the solver calls them at times chosen during integration. The state
solve runs on doubles; the Jacobian path uses Stan Math autodiff. Unsupported
right-hand sides use the fallback described in the appendix. Stanli pays
register dispatch on each solver callback, whereas CmdStan can inline native
right-hand-side code. This cost alone does not determine total gradient time;
the [corpus table](benchmarks.md#full-corpus) reports the resulting timings.

## Retained loops

Unrolling a loop into the graph works when the body is plain arithmetic: the
vector passes recover the vector operations and the data-only parts fold away
at compile time. It stops working when the body carries control that the
compiler cannot decide from data: a `while`, or an `if` on a parameter. Each
iteration then becomes its own runtime island, the islands' reverse pass
snapshots their inputs, and both compile time and gradient time grow much
faster than the model. ctsem's Kalman loop at 200 rows compiled to 347,000
operations and took 80 seconds to prepare this way.

For such loops stanli keeps the loop as one graph operation, `OP_LOOP`. Its
payload is the body as a small tree of control nodes (sequence, if, for,
while, break, continue) over ordinary graph kernels, compiled once from the
same lowering that produces the flat graph. The executor walks the tree at
run time and calls the same kernel functions the flat graph uses, so the
body can use any kernel the flat graph can.

The selector is deliberately narrow. A top-level `while`, or a top-level
counted loop of at least 32 trips whose body, including any user function it
calls, contains a `while` or a branch on a parameter, is retained. A loop the unroller can fold
or vectorize stays unrolled: retaining a loop costs about 25 nanoseconds of
bookkeeping per kernel call, which a vector kernel over the same data beats
by an order of magnitude. `STANLI_STRUCTURED_LOOPS=0` turns retention off and
`=1` retains every loop, for A/B tests.

### What the tape remembers

Reverse mode needs the values a kernel read and wrote, so a retained loop
keeps a tape. Deciding what goes on the tape happens at compile time, per
kernel call in the body:

- A call whose result has no reverse pass and is only read by other such
  calls, or by a branch or loop condition, writes into one fixed cell and is
  overwritten on the next visit.
- A call that feeds a reverse pass appends its inputs' handles, its output
  and any kernel scratch to a growing arena and pushes a record.
- `x[i] = v` on a container the loop owns mutates the container in place and
  logs the overwritten values. The reverse pass undoes the log in order, so
  earlier reads of the container see the values they read. A container that
  arrived from outside the loop, or that another name still refers to, is
  copied once before the first write.
- A call whose inputs no iteration of the enclosing loop writes runs once per
  entry of that loop and is reused afterwards.

This is the same information a hand-written tape would keep, and it is what a
compiled loop body will need: each node's storage is known before the first
iteration runs.

### The first evaluation is recorded and replayed

Much of a real model's loop body is bookkeeping on data: finding a subject's
rows, scanning a setup table, deciding which of several matrices to update.
The unrolled path evaluates all of it at compile time. A retained loop would
otherwise repeat it on every gradient; on ctsem it was 97% of the kernel
calls.

The executor therefore records the first evaluation. While the tree walk
runs, every kernel call, in-place write and container read is appended to a
stream. A call whose inputs are all data is evaluated once and its result
kept as a constant instead of being recorded, and a branch or loop condition
that depends only on data leaves nothing in the stream. When the walk
finishes, the stream is frozen: the records move into compact pools and every
operand becomes a pointer bound once. Later gradients replay the frozen
stream forward and backward without visiting the tree.

Recording also tracks which cells of an updated container still contain data.
Writing a parameter into one cell does not make a read of an untouched data
cell depend on that parameter. A read with constant indices folds when every
selected cell is data; overlapping or parameter-selected reads remain in the
stream. Copy-on-write preserves those facts for aliases, and changing write
indices still trigger the existing guards. This metadata is discarded when
recording finishes. `STANLI_NO_STRUCTURED_CELL_CONSTANTS=1` disables this
per-cell proof for comparisons.

Large recordings use numerical frames. After the existing eight-iteration
sample, a counted loop whose projected version table exceeds 128 MiB and
whose sampled replay averages at least 128 instructions per trip seals the
recorded prefix into a frame, then seals each subsequent iteration as it
completes. The work floor keeps mostly folded loops on ordinary replay.
Sealing copies the live numerical ranges, preserves aliases
and cached values, and replaces the working version table with its live
bindings. Historical derivative identities are independent of those working
handles, including when a later indexed write makes an inactive container
active.

A frame's program uses offsets into its numerical storage and a table of
external bindings. Indexed positions are normalized relative to their first
position; the shift travels with the binding. Frames share a program only
when the complete normalized instructions and position tables match. Every
iteration is still executed and checked during recording; sharing does not
infer an unexecuted row's behavior. Forward and backward use the same kernel
functions and accumulation order as ordinary replay.

Small recordings keep the ordinary stream. `STANLI_STRUCTURED_FRAMES=0`
disables frames, and `=1` forces them on eligible retained plans, including
outer `while` loops. Plans containing register segments keep the established
stream. This choice adds no whole-model preparation pass. The first recording
starts with the control tree. Once frames are selected, the executor compiles
that loop's body into a temporary instruction program with explicit branches
and loop jumps. This avoids repeated recursive tree dispatch while recording.
The program is discarded when recording finishes or throws; warmed replay
continues to use the same frames.

The temporary program also specializes proven data-only transient calls.
A conservative fixed point follows all possible kernel, alias, iterator and
in-place writes from parameter imports. Eligible calls still bind their
current inputs and execute validated operations in the same order,
but skip repeated constant checks and recording bookkeeping. Their output
binding is published only on the first successful execution, so an earlier
read or an untaken branch still sees the original value. Every control guard
remains in place, including a parameter branch choosing between two data
values. Custom or effectful kernels and outputs with alternate writers do
not take this path. `STANLI_STRUCTURED_COMPILED_RECORDING=0` disables the
temporary program; `STANLI_STRUCTURED_DATA_RECORDING=0` disables its data
specialization alone. Both are constructed only after frame admission and
add no analysis or retained storage to ordinary models.

Within that program, canonical data-only point reads with fixed index
geometry validate the complete descriptor on their first successful use.
Later uses refresh the input bindings, check every current selector in the
original validation order, and compute the scalar position directly from
the validated strides. Dynamic extents/counts, other selector kinds and
custom kernels keep the general path. No data address or extra metadata is
cached, and an untaken read cannot throw during program construction.
`STANLI_STRUCTURED_PREPARED_INDEX=0` disables this specialization.

Transient iterators with data-valued bounds and exactly one writer also
reuse their first binding within each loop entry. Subsequent iterations
update only the workspace value. The entry still validates the bounds;
alias/kernel writes, reused iterator slots, parameter-valued bounds and
retained iterators keep ordinary rebinding. Existing branch guards also
cover parameter-controlled choices between data-valued bounds.
`STANLI_STRUCTURED_PREPARED_ITERATOR=0` disables this specialization. Both
proofs run only after frame admission and retain no state after recording.

Parameter-dependent control still runs every time. Each branch, loop
condition and in-place index that depended on a parameter during recording
carries a guard holding the value it took. A replay checks each guard where
it occurs; the first mismatch stops the replay, discards the stream and
records again.
`STANLI_NO_STRUCTURED_REPLAY=1` keeps the tree walk for every evaluation,
for A/B tests.

`write_array` retains the same loops, forward only, so those loops no longer
run on the MIR interpreter for every saved draw.

Before the recording-program optimization, six alternating fresh-process
comparisons with PR391 showed that the mixed-cell
proof and numerical frames reduce ctsem's warmed gradient time from
4.59 to 3.66 ms at 33 rows, 56.88 to 49.09 ms at 400 rows, and 587.01 to
498.87 ms at 4000 rows. At 4000 rows, process peak RSS falls from 10.425 to
1.522 GB, and the first gradient falls from 31.59 to 26.97 seconds. MIR
preparation takes 1.377 seconds in both revisions. Density and gradients
are bitwise identical to PR391 at all three checked parameter points.
In a separate six-pair comparison against that frame implementation, the
temporary recording program cuts the first gradient from 2.71 to 1.96 seconds
at 400 rows and from 26.87 to 19.24 seconds at 4000 rows. Warmed gradients and
preparation remain consistent with parity; live allocation bytes and counts
are exactly equal in every pair after prep, first gradient and replay.
The subsequent index/iterator specializations reduce first gradients from
1.963 to 1.569 seconds at 400 rows and from 19.367 to 15.273 seconds at 4000
rows in another matched six-pair experiment. Preparation, warmed gradients
and peak RSS remain consistent with parity, with identical live bytes and
allocation counts. The [recording-site experiment](superpowers/plans/2026-09-20-ctsem-index-recording.md)
documents the proofs, refusal tests and measurements.
The [experiment record](superpowers/plans/2026-09-19-ctsem-memory.md)
includes smaller cases, ordinary-model controls, build identities and the
remaining first-recording limitation. Detailed ordinary-model timing and
RSS follow-up is archived in [research notes](../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md)
and deferred at the user's request.

## Measured behavior and limits

The [benchmark page](benchmarks.md) reports warm gradients and complete CLI
runs across the application corpus. The [benchmark page](benchmarks.md)
separate model compilation, executor preparation and sampling, and describe
the numerical gates, dispersion and diagnostic warnings.

Computation shape matters more than parameter count:

- Large vector operations and independent observations amortize dispatch.
- Dense linear algebra and ODEs share substantial Stan Math work.
- Sequential models retain scalar dispatch and have less work to batch.

Gradient timing isolates repeated evaluation at a common point. Complete
sampling also depends on adaptation, leapfrog counts and generated quantities;
a faster fixed iteration budget does not establish equal inference quality.

The shared library contains stanc and the precompiled operation vocabulary, so
it is larger than a runtime containing one model. The current size breakdown
is in the [`README.md`](../README.md#binary-size).

Performance is not the numerical oracle. The shared [corpus replay](corpus-status.md)
compares recorded CmdStan log density and every gradient component, with
matching domain refusals and documented ill-conditioned exceptions. Where
available, it also compares complete `write_array` output, including generated
quantities.
Cross-path tests compare graph, register-machine, and MIR/write-array
interpreter configurations. Evidence and known limits are in
[`TESTING.md`](../TESTING.md).

## Appendix: where the MIR interpreter fits

The general MIR interpreter provides language coverage; it is not the main
reason gradient evaluation is fast.

It walks stanc's intermediate representation (called MIR in the source) and
stores variables in an environment instead of prebound graph slots. stanli
uses it for transformed data at model load. The write-array path also has a
whole-section MIR fallback for transformed-parameter and generated-quantity
cases that its forward-only graph cannot lower. ODE right-hand sides and
algebraic-system callbacks have their own register compiler with a MIR
fallback.

MIR is not a general fallback for every log-density lowering failure. If a
graph optimization declines a rewrite, the original graph ops remain.
Parameter-dependent log-density control uses a runtime region. An unsupported
straight-line log-density construct can still raise a compile error. These
details affect language coverage. Except for an ODE right-hand side or
algebraic-system callback that falls back to MIR, these uses do not put the
interpreter in the repeated gradient path.

For a layer-by-layer example with the compiler representation and generated
forward and reverse programs, see
[lowering-walkthrough.md](lowering-walkthrough.md).
