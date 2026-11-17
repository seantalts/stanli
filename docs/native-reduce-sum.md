# Native within-chain reductions

Native builds of stanli can run `reduce_sum` and `reduce_sum_static` in
parallel within a chain. This works from C++, C, Python, R and `stanli_run`.
Build with `-DSTANLI_THREADS=ON`. By default each chain uses one thread. The
web build always runs each chain on one thread.

## Using it

```sh
stanli_run model.stan data.json --chains 4 --num-threads 2 --threads-per-chain 4
```

```python
fit = model.sample(chains=4, parallel_chains=2, threads_per_chain=4)
print(model.reduce_sum_count, model.reduce_sum_fallbacks)
```

```r
fit <- sample_model(model, chains = 4, parallel_chains = 2, threads_per_chain = 4)
fit$model$reduce_sum_count
fit$model$reduce_sum_fallbacks
# Also accepted by sample_cstan() and cstan_model(code)$sample().
```

Each example runs at most two chains at once with four threads each, so up to
eight sampling threads are active. The thread count per chain includes the
chain's own sampling thread. Thread counts must be positive integers.

If you change the thread setting, stanli prepares a new graph before
sampling. Python updates the model in place. R returns the prepared model in
`fit$model`.

Constructors also accept `threads_per_chain`, which applies to direct
gradient calls. Sampling has its own setting, which defaults to one. A thread
count above one needs a thread-safe runtime.

The reduction count is the number of `reduce_sum` calls that stay parallel.
Zero means that every call uses the ordinary serial path. The fallback reasons
say why a call stays serial. Both describe graph lowering only, and
they do not cover opaque runtime regions.

`stanli_run` prints the count and the reasons. If a model with a large
`reduce_sum` runs on one thread, `stanli_run` prints a note about it. The note
comes from a quick size check. A call that the note names may still stay
serial when you request threads.

In C, set `threads_per_chain` in `stanli_model_opts`. Pass the options to
`stanli_model_new_with_opts` or `stanli_model_new_from_stan_with_opts`, which
also take the construction seed. The default is one thread. Each model owns
its team of worker threads. In multi-chain sampling, each executor clone gets
its own team. `stanli_reduce_sum_count`, `stanli_n_reduce_sum_fallbacks` and
`stanli_reduce_sum_fallback` return the same diagnostics.

```cpp
#include <stanli/compile.hpp>
#include <stanli/reduce_sum.hpp>

stanli::CompileOptions options;
options.reduce_sum_threads = 4;
auto model = stanli::compile_model(mir, data, 1, options);
stanli::Executor executor(std::move(model.graph));
model.bind(executor);
stanli::ReduceExecutionContext workers(4);
executor.set_reduce_context(&workers);
// Use executor.gradient(...) or pass executor to the native sampler.
```

In C++, the caller owns the context. The context must outlive the evaluations
that use it. Give each executor that runs concurrently its own context. A
clone rebuilds its private child state and starts with no context attached.

Without a context, a parallel graph runs sequentially, with the same
partition and summation order. If you compile with one thread, stanli uses
the ordinary whole-slice lowering. C++ callers can read the fallback reasons
from `CompiledModel::reduce_sum_fallbacks`.

## Which calls run in parallel

A call can run in parallel if it has fixed shapes, a known positive grainsize
and a pure callback. Stanli must also be able to bound, before running, the
inputs that each child graph reads.

The callback may contain immutable bound and dimension checks. It may also
contain loops over observations that lower to a region map. Partial sums from
brms `threading()` take this form.

These keep a call serial:

- runtime control flow
- dynamic indexing
- opaque or stateful kernels
- side effects in the callback
- child layouts that stanli does not support

Calls inside retained control-flow regions and in generated quantities also
stay serial. A reduction nested inside a parallel outer child runs serially.
Stanli evaluates the arguments and checks the grainsize once per call.

`reduce_sum` uses at most the requested number of chunks.
`reduce_sum_static` uses chunks of exactly the grainsize. A call stays serial
if it has more than `reduce_sum_max_chunks` children (default 1024), if it
has one chunk, or if it has too little work.

Stanli estimates the work from the first child. The estimate is 0.3 ns per
region-map instruction per iteration, plus 0.8 ns for each element that any
other operation touches. Each chunk must cover at least
`reduce_sum_min_chunk_ns` of that estimate (default 30 us). So a small slice
stays serial, and a moderate one uses fewer chunks than threads.

On the four callbacks used for calibration, the estimate was 0.45x to 1.1x of
the measured serial time. The fallback reason shows the estimate. The speedup
you get depends on the cost of the callback and on memory bandwidth. Sending
work to the threads costs about 8 us per thread per gradient.

## Determinism

The chunk count sets the summation order. The chunk count depends on the
data, the model and the thread count. It never depends on thread scheduling.
So runs with the same settings give the same results. Changing the thread
count can change floating-point rounding, and so the sampling trajectory.

Stanli dispatches the forward and reverse passes separately. So the reverse
pass uses the correct incoming adjoint, even inside nonlinear expressions.
Stanli merges the gradients of shared inputs in a fixed child order.

If a job fails, the other jobs in the team still finish. Stanli then reports
the first error in chunk order. A later gradient can reuse the executor.

## Memory

Each child keeps its own values, adjoints, scratch space and gradient buffer.
Stanli stores data arguments that are known at preparation once, in the
reduction. Every child and every chain clone reads that copy. If a child's
body lowers to a region map, the child still copies the ranges that it reads
into its own registers when it runs.

Stanli copies inputs that depend on parameters on each evaluation. It copies
only the ranges that a child reads.

Each chain keeps its worker team, even while the chain is idle. Stanli reuses
buffers and threads across gradients. Kernels and error paths may still
allocate memory.
