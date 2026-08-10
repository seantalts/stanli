# Graph optimizations and performance work

What stanli does to a model's op graph before running it, written for
people who have not worked on compilers. Each section says what the
problem is, what the pass does, and how to turn it off when debugging.
Measurements are in [`docs/benchmarks.md`](../../docs/benchmarks.md).

## Background: why op count matters

stanli turns a Stan model into a list of operations ("ops"). Each op
reads some buffers, computes something, and writes a buffer. The log
density is the list run top to bottom; the gradient is the list run
bottom to top.

Running one op costs roughly 15-20 nanoseconds regardless of how small
the computation inside it is. An op that adds two numbers pays the same
fixed cost as an op that adds two vectors of 10,000 numbers. So the
main way to make a model faster is to make the list shorter: replace
many small ops with few big ones.

The problem is that lowering unrolls loops. A model with

```stan
for (n in 1:1000) {
  y[n] ~ normal(mu[county[n]], sigma);
}
```

becomes several thousand single-number ops. The passes below undo that
without changing the numbers the model computes. They run in this
order, from `lower.cpp`:

1. in-place updates (`inplace.cpp`)
2. store-to-load forwarding (`inplace.cpp`)
3. constant folding (`constfold.cpp`)
4. loop re-rolling (`reroll.cpp`)
5. tape islands (`island.cpp`)

## In-place updates (`inplace.cpp`, disable: `STANLI_NO_INPLACE=1`)

Writing one element of a vector, like `mu[n] = x;` in a loop, lowers as
a "functional update": copy the whole vector into a new buffer, then
change one element. N writes into a length-N vector then copy about
N*N/2 numbers. One real model (radon_county_intercept, N = 12,573)
spent 90 ms per gradient and 2.6 GB of memory this way.

The fix: if nothing ever looks at the old version of the vector again,
change the element directly in the existing buffer. "Nothing looks at
it again" has two parts:

- No later op reads the old version; the write is the last use of that
  buffer.
- No earlier op needs the old values during the gradient step. Some
  ops recompute their derivative from their input buffer when the
  gradient runs, which happens after all the writes. Only ops whose
  gradient step just moves numbers around, without re-reading input
  values, may sit before an in-place write. There is an explicit list
  of those ops (`backward_ignores_input_values`) and a test that checks
  each one really has that property.

The second condition was learned the hard way: without it, eight models
were silently wrong by large amounts with nothing visibly different
about their graphs. The full-corpus comparison at the end of this file
is what caught it.

## Store-to-load forwarding (`inplace.cpp`, same switch)

A common pattern: write `mu[n]`, then read `mu[n]` back in the same
iteration. After lowering that is a write op immediately followed by a
read op on the same element. The pass deletes the read and hands the
value straight to its consumer. If nobody reads the vector at all after
this, the writes are deleted too. This turns "loop that fills a vector"
into "loop that does arithmetic", which re-rolling knows how to
vectorize.

## Constant folding (`constfold.cpp`, disable: `STANLI_NO_CONSTFOLD=1`)

Some parts of a model do not depend on the parameters, but still lower
to ops that recompute the same numbers on every gradient evaluation.
The pass finds every op no parameter can influence, runs those ops
once, saves the results, and deletes the ops.

To run them "once", the pass builds a small temporary graph from just
the constant ops and runs it through the normal executor. This is
deliberate: the kernel is the only definition of what an op computes,
and a separate evaluation path could disagree with it.

One ordering rule: folding replaces a buffer with its final contents.
If a surviving op reads the buffer at a moment when it held different,
earlier contents (this happens with in-place write chains), that buffer
cannot be folded, and nothing computed from it can be either.

## Loop re-rolling (`reroll.cpp`, disable: `STANLI_NO_REROLL=1`)

The main pass. An unrolled loop shows up as the same short op pattern
repeated once per data point; each repetition is called a lane. If the
pass can prove the lanes independent, it replaces them with a handful
of vector ops. To do that it works out how each input varies from lane
to lane:

- Same buffer every lane: keep it; the vector kernels broadcast a
  scalar across elements.
- A different constant every lane: collect the constants into a new
  vector loaded at setup time.
- The output of an earlier op in the same lane: use that op's
  vectorized output.

Element reads and writes get special handling:

- Reading `v[0], v[1], ...` in order across the whole vector: no op at
  all; consumers read `v` directly.
- Reading the same element every lane: one hoisted copy.
- Reading a contiguous range: one slice op.
- Reading arbitrary elements, like `mu[county[n]]`: one gather op. Its
  gradient step adds each element's contribution back to the right
  place, including repeated elements.
- Writing `v[0], v[1], ...` across lanes: one store op, or no op when
  the writes cover the whole vector (the computed vector simply becomes
  `v`). Writes stepping by a fixed stride (filling a matrix column by
  column) use a strided store. When several write runs take turns
  filling one vector, each run's store output becomes the vector
  everything later refers to, so the runs chain and each vectorizes on
  its own.
- A run of writes that all write the same value: keep the computation
  scalar and broadcast it across the range.

Densities get two more rewrites, depending on where their results go.
If every lane's density result goes straight into the target, N scalar
density ops become one vector density op (the vector kernels already
return the summed density). Discrete densities carry their integer
outcome as an attached immediate; lanes fuse by concatenating those
immediates, the form the vector kernel expects. If instead every lane's
density result feeds another op in the same lane (the mixture idiom,
where two densities feed a `log_mix`), the N scalar ops become one
*elementwise* density op whose output holds each lane's own log
density, computed by the same per-element call the scalar ops used, so
the results are bit-identical. The consumers (`log_mix`,
two-argument `log_sum_exp`, ordinary arithmetic) then vectorize as
usual, and one summing op replaces the N per-lane target entries. A
density whose inputs are the same buffer in every lane stays one scalar
op instead.

Anything the pass cannot prove safe it leaves alone. The common
reasons: a lane reads a result computed by the previous lane (a
recurrence involving parameters), a lane's intermediate is used outside
the loop, or an op it has no rule for. When a pattern only partly
qualifies, the pass rewrites the longest qualifying prefix of lanes and
tries again on the rest, which handles data that comes in blocks.

Scale: `radon_pooled` drops from 27,670 ops to 8, `radon_county` from
25,152 to 10, `election88_full` from 289,165 to 65, `dogs` from 12,751
to 261, `low_dim_gauss_mix` to 16.

## Control flow that depends on a parameter (`lower.cpp`, `mir_prog.hpp`)

`if (theta > 0) ...` cannot become ops: the op list is fixed at load
time, and this statement picks its branch anew at every evaluation. The
region (just the conditional, not the model around it) is translated
into the same instruction list the ODE right-hand sides use, and one op
runs it: on plain numbers for the value, and again under stan-math's
own autodiff for the derivatives, which are the derivatives of
whichever branch ran. That is what CmdStan's generated C++ does for the
same statement. This is not an optimization and no switch turns it off;
without it the model does not run.

One refusal: `y ~ normal(mu, 1)` inside such a region is rejected with
a message saying to write `target += normal_lpdf(y | mu, 1)` instead.
The `~` form drops terms based on which arguments are parameters, a
distinction the instruction list cannot make because it binds every
value the same way. Getting it wrong would leave the gradient perfect
and the log density off by a constant, so it is refused rather than
approximated.

Because the region has to translate or the model does not compile at
all, anything the instruction list cannot say is a hard error rather
than a slow path -- which is why its density vocabulary is the runtime's
whole list and not a subset of it. It used to be twelve hand-picked
ones, and `target += chi_square_lpdf(y | nu)` inside an `if` on a
parameter did not compile while the identical line outside the `if`
did. One list now
([`program_density.hpp`](../include/stanli/program_density.hpp)), shared
with the MIR interpreter and with the generated derivatives, so a
density added to the runtime arrives in all three at once.

## Tape islands (`island.cpp`, disable: `STANLI_NO_ISLAND=1`)

Some code cannot be vectorized by anyone: an HMM's forward recursion
computes step t from step t-1, and every step depends on parameters.
Re-rolling correctly refuses these. This pass runs last, so what it
sees is by construction what nothing else could help. It compiles each
long stretch of leftover scalar ops into a single instruction list run
by one op, forward on plain numbers.

The backward used to re-execute that same list under stan-math's
autodiff on a scratch tape and read the derivatives out. That gave the
right answer by construction -- the same arithmetic CmdStan performs for
those statements -- and it cost what CmdStan costs, which was the whole
problem. Collapsing thousands of ops into one sounds like it must be
faster, and with that backward it mostly was not: measured on all
fourteen corpus models with a big enough region, it was faster on one, a
wash on four, and up to 20x slower on nine. The op count was never the
cost; scalar ops are about as cheap as the arithmetic inside them, and
replaying under autodiff is not.

So the backward is *generated* instead (`adjoint.cpp`): the compiler
reads the instruction list once and writes a second instruction list
that computes the derivatives directly, in plain numbers, allocating
nothing. Each rule is the matching stan-math derivative transcribed
expression for expression, so the two still agree to the last bit on
almost every region, and `STANLI_NO_NATIVE_ADJ=1` switches back to the
autodiff replay to check that they do.

That changed the class rather than improving it. On the corpus
models with a region big enough to compile, every one is faster
generated than replayed, and models that were a wash became real wins:
one HMM collapses 42,926 ops into 11 and went from 0.92x to 1.60x. The
model whose steps copy a 1,500-element state vector -- memory traffic
the register file makes free, which is why it was the one winner before
-- went from 2.5x to 4.7x.

The pass still estimates both sides before committing, because one shape
is still wrong for it: a region carrying far more state than it
computes, where the register file costs more to write and read back than
the ops ever moved. The estimate weighs what the ops move and what they
pay per dispatch against the register file and the two instruction
lists, and it keeps every region that measured clearly above parity
while dropping that shape and two more that were slower compiled.
`STANLI_ISLAND_ALWAYS=1` skips it, which is how to ask why a region was
left alone.

Vocabulary is no longer a refusal for scalar work. The machine has its
own instructions for the arithmetic a recurrence is made of, and
anything else with a single-number result -- a cdf, a discrete density,
a special function -- compiles as a *call to the graph's own kernel*,
the exact code the op would have run, partials and derivative included.
So one op the machine has no instruction for stops ending a run, which
used to split regions in half; a call buys continuity, never speed, and
the estimate charges it accordingly. Vector-result ops still end runs.

The pass still refuses outright: short runs (under 32 ops), regions with
more than six distinct inputs, densities in the dropped-constants form
(see the refusal above), and regions producing target entries. One more
applies to the generated backward alone: a region with a branch on a
parameter keeps the autodiff replay, because reversing a branch needs
the nested if/else shape the flat instruction list has already thrown
away.

What is left for this class is the per-instruction cost itself. Both
directions read one instruction at a time and decide what to do with it,
where CmdStan's compiler has inlined the equivalent straight into the
model's machine code.

## Compiled ODE right-hand sides (`ode_prog.cpp`, report fallbacks: `STANLI_DEBUG_ODE=1`)

An ODE model passes a user function (the right-hand side) to a solver
that decides at run time when to call it, so this one function cannot
be unrolled into the graph. It used to be run by walking the compiler's
syntax tree per call, with a map lookup per variable and an allocation
per intermediate: about 6 us per call, hundreds of calls per gradient,
97% of the total time on the ODE models.

Now the function is translated once, at load, into a flat instruction
list: variables become numbered cells, known-bound loops unroll,
run-time conditions become jumps, and a call is one loop over the
instructions with no lookups and no allocation. Anything the translator
cannot handle falls back to the old interpreter (slower, never less
supported); `STANLI_DEBUG_ODE=1` prints when that happens. A test runs
every supported construct through both implementations and requires
bit-identical results.

Separately, the gradient used to solve the ODE system twice, once for
values and once for derivatives. The first solve already computes
everything the derivatives need, so it is kept instead of thrown away
and the second solve is gone.

Together these made the ODE models 29-39x faster.

## Executor details (`executor.cpp`)

- Each op's context (the pointers telling the kernel where its inputs,
  outputs, and scratch are) is built once at setup; nothing moves
  afterwards, so there is nothing to rebuild per evaluation.
- All values live in one arena, all adjoints in another, both allocated
  once. A steady-state gradient evaluation performs no allocation.
- Kernel function pointers are resolved once at setup into two flat
  lists (forward order and reverse order, with backward-less ops left
  out of the second), and both sweeps are unrolled four at a time.

A tail-call threaded dispatch, the usual next step, measured slower
than the unrolled loop (`tools/bench_dispatch.cpp` keeps the
comparison). After these changes per-op cost is dominated by loading
the context, not the dispatch, so the remaining lever is fewer ops,
which is what the passes above are.

## How we check all of this

Every pass changes the graph, and a wrong graph produces wrong numbers
silently. Three layers:

- Unit tests per pass (`tests/test_reroll.cpp`, `test_inplace.cpp`,
  `test_island.cpp`, `test_ode_prog.cpp`, `test_pass_safety.cpp`),
  including tests that the passes refuse what they must refuse, and a
  fuzz test that runs hundreds of random graphs through all passes and
  compares gradients before and after.
- A full-corpus A/B check (`harnesses/ab_corpus.py`): every posteriordb
  model evaluated with the passes off and on, comparing the log density
  and every gradient component. The passes-off graph is separately
  verified against CmdStan, so if A and B agree, the optimized graph
  inherits that verification. This has caught two real bugs the unit
  tests missed. Run it after any change to a pass.
- Direct verification against CmdStan (`tools/verify_sample.py`) for
  models a change affects.

The env switches (`STANLI_NO_INPLACE`, `STANLI_NO_CONSTFOLD`,
`STANLI_NO_REROLL`, `STANLI_NO_ISLAND`) exist so a wrong result can be
attributed to one pass quickly, and they are how each pass is measured:
every speed number is the same build with one variable set and unset.
