# How stanli works and why it is fast

## Abstract

HMC and NUTS evaluate the same log density and its gradient many times. Only
the parameter values change between calls. stanli reads the Stan program and
its data once, prepares a plan for computing the gradient, and reuses that
plan for every gradient.

stanli uses the official `stanc` compiler to parse and optimize your program,
then converts the result into a fixed graph of precompiled operations. When
the model loads, stanli gives each value, each adjoint (reverse-mode
derivative) and each operation's scratch space a fixed place in memory. Each
gradient loads the new parameters and runs the forward and reverse passes
over these arrays of doubles.

Four things account for most of the speedup:

1. No C++ build for each model. stanli does not compile and link generated
   C++, so preparing a model takes much less time.
2. A reverse-mode plan that stanli reuses. stanli builds the graph and most
   derivative storage once. Stan Math rebuilds an autodiff tape for the whole
   model at every gradient.
3. Operations over whole vectors. One graph operation can evaluate a vector
   expression or a density over all observations. The cost of interpreting
   an operation is then paid once per vector, instead of once per element.
4. Simplifying the model with the loaded data. stanli computes expressions
   that depend only on data once, when the model loads. Because it knows the
   loop bounds, shapes, indexes and data-only branches, it can turn scalar
   loops back into broadcasts, slices, gathers and independent batches
   ([example](#partitioning-loops-using-data)).

Models with large vector operations gain the most. So do models with scalar
loops that stanli can turn back into vector operations. The
[benchmark page](benchmarks.md) reports the measured effects.

## What each gradient repeats

At an HMC position `q`, each leapfrog step needs `log p(q)` and its gradient.
It helps to separate the work done once per model from the work done once per
gradient.

Reverse-mode autodiff works in two steps. It first computes values and saves
the local information that the derivatives need. It then sets the derivative
of the result to 1 and applies the chain rule in reverse order. The saved
record is called the autodiff tape.

CmdStan does its work in two parts. Once per model, `stanc` translates the
model to C++, and a C++ compiler builds and links it. Then, at every gradient
call, CmdStan creates the autodiff inputs and records that call's work on
Stan Math's reverse-mode tape. It runs the reverse pass, copies out the
gradient, and frees the autodiff memory.
[`tools/bench_cmdstan_grad.cpp`](../tools/bench_cmdstan_grad.cpp) shows this
sequence. Compiling and recording are separate costs. Compiling fixes the
machine code, but Stan Math still builds the tape each time that code runs.

Stan Math stores each `var`'s `vari` in an arena that hands out memory in
blocks, and `recover_memory()` frees it all at once. So memory itself is
cheap. The cost is in building the tape again at every gradient, and in
writing and reading its records through cache and memory. Vectorized
functions and `var_value` matrices cut the number of records a lot, so there
is not one tape object per observation.

stanli still uses `stanc` for parsing, type checking and Stan-specific
optimization. It replaces the C++ build for each model, and the tape that is
built at each gradient, with a graph that stanli builds once:

| stage | CmdStan with dynamic Stan Math reverse mode | stanli native graph path |
| --- | --- | --- |
| Before a call | Arena blocks and tape-vector capacity may be available for reuse. | Graph, contexts, and value, adjoint, and scratch buffers are already bound. |
| Forward | Compute values while creating autodiff records or callback state. | Write values and partials to predetermined offsets. |
| Reverse | Traverse the work registered during this evaluation and follow its operand references. | Walk a fixed list of reverse ops over contiguous adjoint buffers. |
| Cleanup | Rewind the tape and arena; the retained memory is empty again. | Keep the graph and storage; clear only the required adjoints before the next call. |

The next table gives rough sizes for the four effects, for intuition. They
overlap, and the gain depends on how the model is built:

| mechanism | rough scale | why it helps |
| --- | --- | --- |
| Skip model-specific C++ compilation | Model setup takes milliseconds rather than seconds on small models; the first complete Eight Schools run is roughly 100x faster. | stanli builds a graph in memory instead of calling a C++ compiler and linker. |
| Reuse the reverse-mode plan | Around 2x when rebuilding the autodiff tape is a large part of gradient cost. | Fixed graph and derivative storage replace building the tape again. |
| Execute vector-sized operations | Commonly a few times faster on vector-heavy models; sometimes close to 10x. | One dispatch can process tens, hundreds, or thousands of elements. |
| Specialize and recover loops using data | Several-fold on suitable loop-heavy models. | Data-only work is done once, and scalar iterations can become a few vector batches. |

## From Stan source to a bound operation graph

Consider this statement:

```stan
y ~ normal(mu + tau * theta_tilde, sigma);
```

CmdStan's generated C++ calls arithmetic and probability functions from Stan
Math. What is specific to the model is mostly which functions run, in what
order, with what shapes, and how they connect. stanli stores that arrangement
as data:

```text
MUL          tau, theta_tilde -> scaled       # vector scale
ADD          mu, scaled       -> theta        # scalar broadcast
NORMAL_LPDF  y, theta, sigma  -> target       # summed vector density
```

Each row is an operation, or *op*. An op holds an opcode and indexes into
fixed buffers. The C++ code for each opcode is already compiled into the
stanli shared library.

Preparing a model has four stages:

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

### Storage is assigned once

Binding creates three main arrays:

- The **value arena** holds parameters, data views, intermediate values, and
  the log density.
- The **adjoint arena** holds derivatives for parameters and for
  intermediate values that are still in use. Data, and slots that graph
  optimization removed, need no adjoint storage.
- The **scratch arena** holds partial derivatives and other values that the
  reverse kernels need.

Here, *arena* means a contiguous array whose offsets stanli chooses when the
model loads, unlike Stan Math's dynamic memory pool.

Binding also gives each op direct pointers to its inputs, outputs, adjoints
and scratch. It puts the forward and reverse functions into two flat lists.
The setup code is in [`runtime/src/executor.cpp`](../runtime/src/executor.cpp).

A gradient evaluation is then:

1. Copy the unconstrained parameters into their assigned cells.
2. Run the forward list, computing values and saved partials.
3. Clear the compact adjoint arena and seed the log density with adjoint 1.
4. Run the reverse list, accumulating adjoints.
5. Copy the parameter adjoints to the caller's gradient vector.

stanli does not rebuild names, shapes, offsets, pointers or kernel lookups,
and the executor allocates no memory in this loop.

The kernels that run each op come in several kinds:

- Analytic kernels work directly on arrays of doubles. They save what the
  reverse pass needs in fixed scratch.
- Generic probability kernels use a small recording scalar type
  ([`recorder.hpp`](../runtime/include/stanli/recorder.hpp)), so that the
  existing Stan Math template computes its usual partial derivatives. The
  kernel reads vector arguments in place and writes the partials of a vector
  input straight into scratch. The Stan Math code still allocates its own
  Eigen temporaries.
- `normal_lpdf`, `cauchy_lpdf`, `student_t_lpdf`, `lognormal_lpdf`,
  `beta_lpdf`, `gamma_lpdf` and `ordered_logistic_lpmf` (shared cutpoints)
  have fused kernels
  ([`density_fused.cpp`](../runtime/kernels/density_fused.cpp)).
  `normal_lpdf` allocates nothing. `cauchy_lpdf` allocates one work array
  above 512 elements, and only when both the outcome and the location are
  parameters. The others use stack scratch up to 128 elements and one work
  array above that.
- A function with no analytic or recorder path builds a nested Stan Math
  tape inside its kernel
  ([`legacy.hpp`](../runtime/include/stanli/legacy.hpp)). This keeps
  language coverage, and the cost of dynamic autodiff stays inside that one
  kernel.

So preallocation covers the arenas that the executor owns and the scratch
that each op declares. Some kernels still allocate temporaries.

## Why operation-level interpretation is cheap enough

An interpreter adds a cost to dispatch each operation. What matters is how
much work each dispatch does.

The microbenchmark in [`tools/bench_opcost.cpp`](../tools/bench_opcost.cpp)
measures roughly 17 to 20 ns to run one scalar density op forward and
backward, including the overhead of the executor and the recorder. The
density arithmetic alone takes about 0.9 ns on that machine. So a graph with
thousands of scalar ops will be slow.

A vector op changes this ratio. One `NORMAL_LPDF` op can process 1,000
observations in compiled C++ and pay the dispatch cost once. Its kernels loop
over contiguous values and partials, with no graph metadata per element.
Eigen or the compiler may use SIMD instructions in some kernels, but stanli
does not require SIMD. Some kernels keep a scalar order of summation so that
their results agree with Stan Math.

CmdStan has no interpreter dispatch. But its reverse mode still builds and
traverses the autodiff state that each expression needs. For models with many
medium or large vector operations, that cost can be larger than stanli's
dispatch cost. When one large Cholesky factorization or matrix multiplication
dominates a model, both runtimes spend most of their time in the same
numerical code, so their speed is usually similar.

## Recovering vector operations from scalar loops

Explicit loops are common in Stan and are often clearer than hand-vectorized
code. When the loop bounds and indexes are known from the data, stanli first
expands the loop during lowering. For example:

```stan
for (n in 1:N)
  y[n] ~ normal(alpha[county[n]], sigma);
```

This first produces one copy of the loop body per observation. This page
calls each copy a *lane*:

```text
INDEX        alpha, county[1] -> a1
NORMAL_LPDF  y[1], a1, sigma  -> lp1

INDEX        alpha, county[2] -> a2
NORMAL_LPDF  y[2], a2, sigma  -> lp2

...
```

Left like this, the graph would pay interpreter overhead for every
observation. The re-roll pass in
[`runtime/src/reroll.cpp`](../runtime/src/reroll.cpp) finds repeated
patterns. It proves that the lanes do not depend on each other, and then
sorts their inputs into kinds:

- `sigma` is the same slot in every lane, so it stays a scalar and is
  broadcast.
- `y[n]` does not depend on parameters. Constant folding removes the scalar
  reads, and re-rolling collects the values into one vector at load time.
- `alpha[county[n]]` uses indexes known from the data, so its scalar reads
  become one gather. Repeated county indexes are allowed. The gather's
  reverse pass adds their contributions into the matching `alpha` elements.
- A value computed inside each lane becomes a vector that feeds the next
  vector op.

The rewritten graph is approximately:

```text
GATHER       alpha, county[1:N] -> county_alpha
NORMAL_LPDF  y, county_alpha, sigma -> target
```

This graph works on whole vectors, as a hand-vectorized Stan statement would.

Re-rolling also recognizes slices, arbitrary gathers, repeated writes to
elements, values that all lanes share, and data constants that differ across
lanes. In mixture models, it keeps the log density of each observation
separate through `log_mix` or `log_sum_exp` and adds them up only at the end.

stanli does not re-roll the loop in three cases. One lane reads a
parameter-dependent result from the previous lane. An intermediate value is
used outside the region in an unsupported way. Or an operation is unsupported
or has side effects. A recurrence is sequential work and must stay
sequential.

### Partitioning loops using data

Sometimes independent lanes are not next to each other, because the data
selects different work for each observation. For example:

```stan
for (n in 1:N) {
  if (group[n] == 1)
    y[n] ~ normal(mu_1, sigma_1);
  else
    y[n] ~ student_t(nu, mu_2, sigma_2);
}
```

If `group` is data, stanli knows the outcome of every branch when the model
loads. When the rewrite is safe and worth doing, stanli groups the
observations that take the same branch and vectorizes each group separately.
In this example, the interleaved scalar branches become one vectorized normal
likelihood and one vectorized Student-t likelihood.

This works only when the condition depends on data alone. If the branch
depends on a parameter, its outcome can change at every gradient evaluation,
so it must stay as runtime control flow.

### The graph passes that support loop recovery

Several passes expose and preserve vector structure:

| pass | purpose |
| --- | --- |
| Safe in-place updates | Avoid copying a whole vector for assignments such as `v[n] = x` when no later forward or reverse work needs the old value. This keeps `N` element assignments from causing quadratic copying. |
| Store-to-load forwarding | Remove a write that is followed immediately by a read of the same element. This exposes the arithmetic lane underneath. |
| Constant folding | Evaluate graph regions that depend only on loaded data. |
| Re-rolling | Combine independent lanes that are adjacent and repeat with a fixed period. |
| Lane partitioning | Combine equivalent independent lanes that are interleaved or out of phase. |
| Common-subexpression elimination | Remove repeated work that the structural rewrites expose. |
| Islands | Combine scalar regions that cannot become vector operations, when that is worth doing. |
| Observation collapse | In [fast mode](fast-mode.md) only, evaluate a repeated likelihood once per distinct row or group. |

stanli runs the in-place cleanup again after re-rolling and partitioning, to
handle the slice and strided writes that those passes create. Islands run
after the vector passes. Target terms are summed only after all these
rewrites. [`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md)
describes each pass, when it is safe, its diagnostic switch and its A/B
measurements. [`runtime/src/lower.cpp`](../runtime/src/lower.cpp) sets the
order of the passes.

Two models show the scale of the structural changes:

- `radon_pooled` falls from 27,670 graph ops to 8. Re-rolling alone took
  its gradient from 0.91x to 6.18x CmdStan's speed.
- `election88_full` falls from 289,165 ops to 65.

The [benchmark table](benchmarks.md#full-corpus) has current timings for both.

Re-rolling can change the order in which floating-point sums are added, so
the optimized graph and the scalar graph may differ in their last few bits.
Differential and cross-path tests check for exact equality where it is
expected, and for documented bounds otherwise; see
[`TESTING.md`](../TESTING.md).

## Scalar recurrences and generated reverse programs

Not every loop is independent. An HMM forward recursion computes the state at
time `t` from the state at time `t - 1`. Evaluating all time steps as one
vector would change the calculation.

After the vector passes, stanli may combine a scalar region into one
*island*, when that is worth doing. An island is a compact list of
instructions over numbered registers. stanli fixes the register locations and
sizes when the model loads. The forward pass then runs on plain doubles and
does not look up graph context for each scalar instruction. An island with a
generated adjoint keeps its forward values and checkpoints in preallocated
scratch. Its backward pass uses a reusable adjoint buffer for each thread.
That buffer may grow on the first call that needs more room.

Most of the gain comes from the island's reverse pass. Earlier, stanli
replayed each island under `stan::math::var` at every gradient. That built an
inner autodiff tape, so most models did not get faster and several got slower
even though the outer graph had fewer ops. Now, for an eligible
island with no branches, stanli generates a second instruction list at model
load. This list computes the pullback (the reverse-mode derivative) directly
on doubles ([`runtime/src/adjoint.cpp`](../runtime/src/adjoint.cpp)).

This gives a direct comparison with building a tape at each call. In the
targeted A/B test of islands, `hmm_gaussian` falls from 42,926 graph ops to
11. Replaying the island with a nested Stan Math tape measured 0.92x the
graph baseline. The generated reverse program measured 1.60x. Replay reran the
forward calculation while it built and traversed its nested tape. The
generated path ran only the derivative program.

Islands still pay dispatch for each scalar instruction, so they are likely to
gain less than vector operations do. A cost model leaves a region as ordinary
graph ops when it has too little work or too much register traffic.

When control flow depends on a parameter, stanli uses a runtime region, where
the sequence of branches may change between evaluations. The generated
reverse pass does not apply there, so that path replays with nested autodiff.

ODE right-hand sides that the register compiler accepts use a related
program, because the solver calls them at times that it picks during
integration. The state solve runs on doubles, and the Jacobian uses Stan Math
autodiff. Unsupported right-hand sides fall back to the MIR interpreter
described in the appendix. stanli pays register dispatch on every call from
the solver, while CmdStan can inline native code for the right-hand side. The
[corpus table](benchmarks.md#full-corpus) reports the resulting timings.

## Retained loops

Unrolling a loop into the graph works when the body is plain arithmetic. The
vector passes recover the vector operations, and the data-only parts fold
away. It stops working when the body has control flow that the data does not
decide, such as a `while` or an `if` on a parameter. Each iteration would then
become its own runtime island, and both compile time and gradient time grow
much faster than the model. Unrolled this way, ctsem's Kalman loop at 200
rows compiled to 347,000 operations and took 80 seconds to prepare.

For such loops, stanli keeps the loop as one graph operation, `OP_LOOP`. This
op holds the loop body as a small tree of control nodes (sequence, if, for,
while, break, continue) over ordinary graph kernels. The same lowering that
builds the flat graph produces this tree, so the body can use any kernel that
the flat graph can. stanli keeps a loop this way in two cases:

- a top-level `while`;
- a counted loop of at least 32 trips whose body contains a `while` or a
  branch on a parameter. The body here includes any user function that it
  calls.

A loop that stanli can fold or vectorize stays unrolled. A vector kernel over
the same data is an order of magnitude faster than the bookkeeping that the
loop does on each call.

`write_array` keeps one more kind of loop as a single op. The loop has at
least 32 trips and control flow that depends only on data, and each
iteration reads what an earlier iteration wrote. stanli does this only once
the `write_array` graph already holds 65,536 operations. The vector passes
cannot fold such a chain of dependent steps. In a graph that large, each
unrolled operation also costs about three times more per draw than in a small
graph. For covid19imperial, the generated quantities go from 587,557
operations to 45,581. Preparation is 5.5x faster, peak memory is 3.6x lower,
and draws are 2.6x faster. The first draw records the loops and takes about
50 ms.

A retained loop keeps its own tape. stanli decides at compile time how to
store each kernel call in the body:

- A call whose result does not feed a reverse pass writes into one fixed
  cell.
- A call whose result does feed a reverse pass appends its operands and
  scratch to an arena that grows.
- An indexed write into a container that the loop owns changes it in place.
  stanli logs the old value so that the reverse pass can undo the write.
- A call whose inputs no iteration writes runs once each time the loop
  starts.

stanli records the first evaluation. Every kernel call, in-place write and
container read goes into a stream. A call whose inputs are all data is
evaluated once and kept as a constant. A condition that depends only on data
leaves nothing in the stream. On ctsem, 97% of the kernel calls were data
bookkeeping. stanli then replays the frozen stream forward and backward,
without walking the tree.

During recording, some branches, loop conditions and indexes depend on a
parameter. Each of these gets a guard that holds the value it took. During
replay, stanli checks each guard when it reaches it. At the first mismatch,
stanli discards the stream and records again. `write_array` replays the same
loops forward only. stanli stores large recordings as numerical frames, one
per iteration.
[`OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md#retained-loops)
describes how the frames work, the specializations made during recording,
their switches and the ctsem measurements.

## Measured behavior and limits

The [benchmark page](benchmarks.md) reports setup time and warm gradients
across the application corpus.
[How we measure](benchmarks.md#how-we-measure) describes the numerical check
that each pair must pass.

The shape of the computation matters more than the number of parameters.
With large vector operations and independent observations, the dispatch cost
is spread over many elements. Dense linear algebra and ODEs share most of
their Stan Math work with CmdStan. Sequential models keep scalar dispatch and
have less work to batch.

The shared library contains stanc and all the precompiled operations, so it
is larger than a runtime that contains one model.
[Install size](install-size.md) has the size breakdown.

stanli tests correctness separately from performance. The
[corpus replay](corpus-status.md) compares stanli with recorded CmdStan
output: the log density, every gradient component and, where available, the
complete `write_array` output, including generated quantities. Cross-path
tests compare the graph, register-machine and MIR interpreter paths with each
other. [`TESTING.md`](../TESTING.md) has the evidence and the known limits.

## Appendix: where the MIR interpreter fits

The general MIR interpreter gives stanli its language coverage. Gradient
speed comes from the graph. The interpreter walks stanc's intermediate
representation (called MIR in the source). It keeps variables in an
environment instead of in prebound graph slots. stanli uses it in three
places:

- transformed data, at model load;
- a whole transformed parameters or generated quantities section, when the
  forward-only `write_array` graph cannot lower that section;
- ODE right-hand sides and algebraic-system callbacks that the register
  compiler does not accept.

MIR is not a general fallback for the log density. If a graph optimization
does not apply, the original graph ops stay. Log-density control flow that
depends on parameters uses a runtime region. A straight-line log-density
construct that stanli does not support gives a compile error. Apart from
callbacks that fall back to MIR, none of these puts the interpreter in the
repeated gradient path.

For a layer-by-layer example with the compiler representation and generated
forward and reverse programs, see
[lowering-walkthrough.md](lowering-walkthrough.md).
