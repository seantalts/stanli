# Native within-chain reductions

Native C++, C, Python, R and `stanli_run` can run `reduce_sum` and
`reduce_sum_static` in parallel within a chain. Build with
`-DSTANLI_THREADS=ON`. The default is one thread per chain, and the web build
stays serial within each chain.

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

Changing the thread setting prepares a new graph before sampling. Python
updates the model in place; R returns the prepared model in `fit$model`.
Constructors also accept `threads_per_chain` for direct gradient calls;
sampling has its own setting, which defaults to one. A thread count above one
needs a thread-safe runtime.

The reduction count says how many `reduce_sum` calls were kept parallel; zero
means every call uses the ordinary serial path. The fallback reasons say why a
call was refused. Both describe graph lowering and do not cover opaque runtime
regions. The CLI prints the count and the reasons. When a model with a large
`reduce_sum` runs at one thread, the CLI prints a note saying so. The note is a
cheap size check, so a call it names may still be refused when threads are
requested.

C callers set `threads_per_chain` in `stanli_model_opts` and pass it to
`stanli_model_new_with_opts` or `stanli_model_new_from_stan_with_opts`, which
also take the construction seed. The model owns its worker team, and
multi-chain sampling gives each executor clone its own team. The default is one
thread. `stanli_reduce_sum_count` and `stanli_reduce_sum_fallbacks` return the
same diagnostics.

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

In C++ the context belongs to the caller and must outlive the evaluations that
use it. Give each executor that runs concurrently its own context. Clones
rebuild their private child state and start with no context attached. A
parallel graph also runs sequentially without a context, with the same
partition and summation order. Compiling with one thread selects the ordinary
whole-slice lowering. C++ callers can read refusal reasons from
`CompiledModel::reduce_sum_fallbacks`.

## Which calls run in parallel

An eligible call has fixed shapes, a known positive grainsize, a pure
callback, and child graphs whose input reads can be bounded statically.
Immutable bound and dimension checks are allowed, as are per-observation loops
that lower to a region map, which is the form brms `threading()` partial sums
take. Runtime control flow, dynamic indexing, opaque or stateful kernels,
callback side effects and unsupported child layouts keep the serial lowering.
So do calls inside retained control-flow regions and generated quantities.
Nested reductions run serially inside a parallel outer child. Arguments are
evaluated and the grainsize validated once per call.

`reduce_sum` uses at most the requested number of chunks, and
`reduce_sum_static` uses chunks of exactly the grainsize. Calls with more than
`reduce_sum_max_chunks` children (default 1024), with one chunk, or with too
little work stay serial. Work is estimated from the first child as 0.3 ns per
region-map instruction per iteration plus 0.8 ns per element touched by any
other operation. Each chunk must cover at least `reduce_sum_min_chunk_ns` of
that estimate (default 30 us), so a small slice stays serial and a moderate one
uses fewer chunks than threads. On the four callbacks used for calibration, the
estimate ranged from 0.45x to 1.1x of the measured serial time; the refusal
reason prints it. The actual benefit depends on callback cost and memory
bandwidth. Dispatch costs about 8 us per thread per gradient.

## Determinism

The chunk count sets the summation order, so it depends on the data, the model
and the thread count, and never on thread scheduling. Results are the same
across runs with the same settings. Changing the thread count can change
floating-point rounding and therefore the sampling trajectory.

Forward and reverse passes dispatch separately, so the reverse pass uses the
actual incoming adjoint even inside nonlinear expressions. Gradients of shared
inputs merge in fixed child order. A failed job lets the team finish, then
reports the first error in chunk order; a later gradient can reuse the
executor.

## Memory

Each child keeps its own values, adjoints, scratch and gradient buffer. Data
arguments known at preparation are stored once in the reduction, and every
child and chain clone reads that copy. A child whose body lowers to a region
map still copies the ranges it reads into its own registers when it runs.
Parameter-dependent inputs are copied on each evaluation, limited to the
ranges a child reads. Worker teams persist for each chain, including while it
is idle, and buffers and threads are reused across gradients; kernels and
error paths may still allocate.
