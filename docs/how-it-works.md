# How stanli works and why it is fast

## Abstract

HMC and NUTS evaluate the same log density and gradient many times; only the
parameter values change. stanli prepares a reusable execution plan once,
after reading the Stan program and its data, and runs that plan for every
gradient.

stanli runs the official `stanc` front end, then lowers the optimized program
to a fixed graph of precompiled C++ operations. At model load it assigns fixed
locations to graph values, graph adjoints (reverse-mode derivative
accumulators), and each operation's declared scratch space. A gradient then
loads new parameters and runs the forward and reverse sweeps over double
buffers.

Four mechanisms provide most of the speedup:

1. **No model-specific C++ build.** stanli does not compile and link generated
   C++, so model preparation takes milliseconds.
2. **A reusable reverse-mode plan.** The graph and most derivative storage are
   built once, instead of reconstructing a model-wide Stan Math autodiff tape
   for every gradient.
3. **Vector-sized operations.** One graph operation can evaluate a vector
   expression or density over all observations, so interpreter dispatch is
   paid per vector operation rather than per scalar element.
4. **Specialization using the loaded data.** Data-only expressions are
   computed once. Known loop bounds, shapes, indexes, and data-only branches
   let stanli recover broadcasts, slices, gathers, and independent batches
   from scalar loops.

Models with large vector operations, or scalar loops stanli can turn back into
vector operations, gain the most. Models dominated by one large Stan Math
kernel have less interpreter overhead to remove. The
[benchmark page](benchmarks.md) reports the measured effects.

## What each gradient repeats

At an HMC position `q`, each leapfrog step needs `log p(q)` and its gradient.
Parameters change between calls; the data, declared shapes, and most of the
computation do not. The useful distinction is work done once per model and
work done once per gradient.

CmdStan does two things. `stanc` translates the model to C++ and a C++
compiler builds and links it, once per model. Then each gradient call creates
the autodiff inputs, records the work for that call on Stan Math's
reverse-mode tape, runs the reverse pass, copies the gradient, and recovers
the autodiff arena. [`tools/bench_cmdstan_grad.cpp`](../tools/bench_cmdstan_grad.cpp)
shows this sequence directly. Native compilation and autodiff recording are
separate costs: compiling fixes the machine code, and Stan Math still
constructs the tape each time that code runs.

Stan Math's arena makes obtaining and reclaiming bytes cheap. A `var` refers
to a `vari` in an arena that hands out memory in blocks, and
`recover_memory()` resets it in bulk. What the arena does not preserve is a
constructed tape. Each gradient still creates and initializes records or
callbacks, stores operand references and partial derivatives, registers the
reverse work, writes and later reads those records through cache and memory
bandwidth, traverses them in reverse, and resets the tape. Vectorized Stan
Math functions and `var_value` matrices reduce the record count a lot; the
cost being compared is repeated construction and metadata, and Stan Math does
not use one tape object per observation.

stanli keeps `stanc` for parsing, type checking, and Stan-specific
optimization. It replaces the model-specific C++ build and the per-gradient
tape with a graph built once:

| stage | CmdStan with dynamic Stan Math reverse mode | stanli native graph path |
| --- | --- | --- |
| Before a call | Arena blocks and tape-vector capacity may be available for reuse. | Graph, contexts, and value, adjoint, and scratch buffers are already bound. |
| Forward | Compute values while creating autodiff records or callback state. | Write values and partials to predetermined offsets. |
| Reverse | Traverse the work registered during this evaluation and follow its operand references. | Walk a fixed reverse-op list over contiguous adjoint buffers. |
| Cleanup | Rewind the tape and arena; the retained memory is empty again. | Keep the graph and storage; clear only the required adjoints before the next call. |

Rough scales for the four mechanisms, for intuition; they overlap, and the
gain depends on model structure:

| mechanism | rough scale | why it helps |
| --- | --- | --- |
| Skip model-specific C++ compilation | Model setup takes milliseconds rather than seconds on small models; the first complete Eight Schools run is roughly 100x faster. | stanli builds a graph in memory instead of invoking a C++ compiler and linker. |
| Reuse the reverse-mode plan | Around 2x when rebuilding the autodiff tape is a substantial part of gradient cost. | Fixed graph and derivative storage replace repeated tape construction. |
| Execute vector-sized operations | Commonly a few times faster on vector-heavy models; sometimes close to 10x. | One dispatch can process tens, hundreds, or thousands of elements. |
| Specialize and recover loops using data | Several-fold on suitable loop-heavy models. | Data-only work is done once, and scalar iterations can become a few vector batches. |

Constant folding is the simplest form of data specialization: an expression
that depends only on data is evaluated once at model load. Loaded data can
also determine graph structure. If `group[n]` is data and selects one of two
likelihoods inside a loop, every branch outcome is fixed before sampling
starts, so stanli can split the observations into two independent sets and
run one vectorized likelihood per set.
[Partitioning loops using data](#partitioning-loops-using-data) works the
example.

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

This is still compilation in the broad sense, since the model is analyzed and
converted to a lower-level representation, but no model-specific native C++
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

Here, *arena* means a contiguous array whose offsets are chosen at model load,
unlike Stan Math's dynamic autodiff memory pool.

Binding also gives each op direct input, output, adjoint, and scratch
pointers, and resolves the forward and reverse functions into two flat lists.
The setup code is in [`runtime/src/executor.cpp`](../runtime/src/executor.cpp).

A gradient evaluation is then:

1. Copy the unconstrained parameters into their assigned cells.
2. Run the forward list, computing values and saved partials.
3. Clear the compact adjoint arena and seed the log density with adjoint 1.
4. Run the reverse list, accumulating adjoints.
5. Copy the parameter adjoints to the caller's gradient vector.

Names, shapes, offsets, pointers, and kernel lookup are not rebuilt, and the
executor allocates no memory in this loop.

Several kernel paths sit behind that interface. Analytic kernels operate
directly on double buffers and save what the reverse pass needs in fixed
scratch. Generic probability kernels use a small recording scalar so the
existing Stan Math template computes its usual partial derivatives
([`recorder.hpp`](../runtime/include/stanli/recorder.hpp)); for a vector
input that depends on parameters, this path builds a temporary Eigen array of
partials before copying it to scratch. Functions without an analytic or
recorder path build a nested Stan Math tape inside the kernel
([`legacy.hpp`](../runtime/include/stanli/legacy.hpp)), which preserves
coverage but keeps dynamic autodiff cost locally. Preallocation therefore
covers executor-owned arenas and declared op scratch, and some kernels still
allocate temporaries.

## Why operation-level interpretation is cheap enough

Interpretation adds dispatch overhead; what matters is how much work each
dispatch performs.

The microbenchmark in [`tools/bench_opcost.cpp`](../tools/bench_opcost.cpp)
measures roughly 17 to 20 ns for one scalar density op's complete forward and
backward execution, including executor and recorder overhead. The density
arithmetic alone takes about 0.9 ns on that machine. A graph with thousands of
scalar ops will therefore be slow.

A vector op changes the ratio. One `NORMAL_LPDF` op can process 1,000
observations in compiled C++ while paying graph dispatch once. Its kernels
loop over contiguous values and partials with no graph metadata per element.
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
`log_mix` or `log_sum_exp` and reduces only at the end.

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
slice and strided stores those passes create. Islands run after the vector
passes, and target terms are reduced only after all these rewrites. The
passes, their safety conditions, their diagnostic switches and their A/B
measurements are in [`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md);
their order is implemented in [`runtime/src/lower.cpp`](../runtime/src/lower.cpp).

Two models show the scale of the structural changes:

- `radon_pooled` falls from 27,670 graph ops to 8. In the targeted re-roll
  A/B it moved from 0.91x to 6.18x CmdStan; the archived September 11 run
  records 7.35x for the complete system.
- `election88_full` falls from 289,165 ops to 65. The same archived run
  records 4.16x CmdStan.

The targeted A/B isolates one optimization; the
[archived corpus rows](../notes/performance/2026-09-11-posteriordb-benchmark.md#historical-posteriordb-model-results)
record those complete-system measurements, and the
[current benchmark](benchmarks.md) measures the current build.

Re-rolling can change the association order of floating-point reductions, so
the optimized and scalar graphs may differ in their last few bits.
Differential and cross-path tests check exact equality where expected and
documented bounds otherwise; see [`TESTING.md`](../TESTING.md).

## Scalar recurrences and generated reverse programs

Not every loop is independent. An HMM forward recursion computes state at
time `t` from state at time `t - 1`; evaluating all time steps as a vector
would change the calculation.

After the vector passes, stanli may combine a profitable scalar region into
one *island*: a compact instruction list over numbered registers. Register
locations and sizes are fixed at model load, so the forward pass runs on plain
doubles without a graph-context lookup per scalar instruction. A
generated-adjoint island keeps its forward values and checkpoints in
preallocated scratch; its backward pass uses a reusable thread-local adjoint
buffer that may resize on its first sufficiently large call.

The main optimization is the island's reverse pass. An earlier implementation
replayed each island under `stan::math::var` for every gradient. That reduced
the outer graph's op count but built an inner autodiff tape, so most models
did not improve and several became slower. For an eligible branch-free island,
stanli now generates a second instruction list at model load that computes the
pullback directly on doubles ([`runtime/src/adjoint.cpp`](../runtime/src/adjoint.cpp)).

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
right-hand sides fall back to the MIR interpreter described in the appendix.
stanli pays register dispatch on each solver callback, whereas CmdStan can
inline native right-hand-side code; the [corpus table](benchmarks.md#full-corpus)
reports the resulting timings.

## Retained loops

Unrolling a loop into the graph works when the body is plain arithmetic: the
vector passes recover the vector operations and the data-only parts fold
away. It stops working when the body carries control the compiler cannot
decide from data, a `while` or an `if` on a parameter. Each iteration would
become its own runtime island, and both compile time and gradient time grow
much faster than the model; ctsem's Kalman loop at 200 rows compiled to
347,000 operations and took 80 seconds to prepare that way.

For such loops stanli keeps the loop as one graph operation, `OP_LOOP`. Its
payload is the body as a small tree of control nodes (sequence, if, for,
while, break, continue) over ordinary graph kernels, produced by the same
lowering as the flat graph, so the body can use any kernel the flat graph
can. The selector is narrow: a top-level `while`, or a counted loop of at
least 32 trips whose body, including any user function it calls, contains a
`while` or a branch on a parameter. A loop the unroller can fold or vectorize
stays unrolled, because a vector kernel over the same data beats the loop's
per-call bookkeeping by an order of magnitude.

A retained loop keeps its own tape, decided at compile time per kernel call
in the body. A call whose result feeds no reverse pass writes into one fixed
cell. A call that does feed one appends its operands and scratch to a growing
arena. An indexed write into a container the loop owns mutates it in place
and logs the overwritten value for the reverse pass to undo. A call whose
inputs no iteration writes runs once per loop entry.

The first evaluation is recorded. Every kernel call, in-place write and
container read goes into a stream; a call whose inputs are all data is
evaluated once and kept as a constant, and a data-only condition leaves
nothing in the stream. On ctsem, data bookkeeping was 97% of the kernel
calls. The frozen stream is then replayed forward and backward without
visiting the tree. Each branch, loop condition and index that depended on a
parameter during recording carries a guard holding the value it took; a
replay checks the guards where they occur, and the first mismatch discards
the stream and records again. `write_array` replays the same loops forward
only. Large recordings are sealed into per-iteration numerical frames; the
frame mechanics, the recording-time specializations, their switches and the
ctsem measurements are in
[`OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md#retained-loops).

## Measured behavior and limits

The [benchmark page](benchmarks.md) reports setup and warm gradients across
the application corpus, with the numerical gate each pair must pass.
Computation shape matters more than parameter count: large vector operations
and independent observations amortize dispatch, dense linear algebra and ODEs
share most of their Stan Math work with CmdStan, and sequential models keep
scalar dispatch with less to batch.

The shared library contains stanc and the precompiled operation vocabulary, so
it is larger than a runtime containing one model. The size breakdown is in
the [`README.md`](../README.md#binary-size).

Performance is measured separately from correctness. The
[corpus replay](corpus-status.md) compares recorded CmdStan log density,
every gradient component and, where available, complete `write_array`
output; cross-path tests compare stanli's own execution paths with each
other. Evidence and known limits are in [`TESTING.md`](../TESTING.md).

## Appendix: where the MIR interpreter fits

The general MIR interpreter provides language coverage; it is not why
gradient evaluation is fast. It walks stanc's intermediate representation
(called MIR in the source) and stores variables in an environment instead of
prebound graph slots. stanli uses it for transformed data at model load, as a
whole-section fallback for transformed-parameter and generated-quantity cases
the forward-only write-array graph cannot lower, and as the fallback for ODE
right-hand sides and algebraic-system callbacks the register compiler
refuses.

MIR is not a general fallback for log-density lowering. If a graph
optimization declines a rewrite, the original graph ops remain;
parameter-dependent log-density control uses a runtime region; an unsupported
straight-line log-density construct raises a compile error. Except for a
callback that falls back to MIR, none of these uses puts the interpreter in
the repeated gradient path.

For a layer-by-layer example with the compiler representation and generated
forward and reverse programs, see
[lowering-walkthrough.md](lowering-walkthrough.md).
