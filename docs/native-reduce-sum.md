# Native within-chain reductions

Native C++, C, Python, R and `stanli_run` can opt into within-chain `reduce_sum` and
`reduce_sum_static` parallelism. Build with `-DSTANLI_THREADS=ON`.

```sh
stanli_run model.stan data.json --chains 4 --num-threads 2 --threads-per-chain 4
```

`--num-threads` limits simultaneously sampled chains; `--threads-per-chain`
includes each chain's sampling thread. This example uses up to eight active
sampling threads. Worker teams persist for each chain, including while that
chain is idle. The CLI reports how many reduction sites were retained. Zero
means all sites use the ordinary serial path. The CLI prints graph-lowering
refusal reasons; C++ callers can inspect `CompiledModel::reduce_sum_fallbacks`.
The default remains one thread. The Web interface remains serial within each chain.

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

These examples use up to eight active sampling threads. Changing the thread
setting prepares a new graph before sampling; worker teams and child storage
persist across gradients. Python updates the model in place; R returns the
prepared model in `fit$model`. Construction also accepts `threads_per_chain`
for direct gradient calls. Sampling has its own setting, defaulting to one.
Counts and refusal reasons describe graph lowering, not opaque runtime regions.
A count of zero means the model has no retained native parallel reductions.
Thread counts must be positive integers. Values above one require a thread-safe
runtime and the new constructor entry points; older runtimes still support
serial calls but reject requests for within-chain parallelism.

C callers use `stanli_model_new_threaded` or
`stanli_model_new_from_stan_threaded`, supplying the construction seed and thread
count. The model owns its worker team, and multi-chain sampling supplies teams
for its executor clones. Existing C constructors and sampling structs keep their
ABI and serial defaults. `stanli_reduce_sum_count` and
`stanli_reduce_sum_fallbacks` expose the same diagnostics.

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

The context is caller-owned and must outlive evaluations using it. Give each
concurrently evaluated executor its own context. Clones rebuild private child
state and start without an attached context. A retained graph also works
sequentially without a context, with the same partition and summation order.
Compiling with one thread selects the established whole-slice lowering instead.

Eligible calls have fixed shapes, a known positive grainsize, pure callbacks,
and transparent child graphs whose input reads can be bounded statically.
Runtime control or dynamic indexing, opaque/stateful kernels, callback effects,
and unsupported child layouts conservatively keep the ordinary lowering.
Immutable bound/dimension-check payloads are supported and stay in the child.
Argument evaluation and grainsize validation still occur once per call.
Nested reductions run serially within a retained outer child. Calls in retained
control-flow regions and generated quantities currently keep their serial path.

Ordinary reductions use at most the requested compile-time number of chunks;
static reductions use deterministic grainsize-sized chunks. Child counts above
`reduce_sum_max_chunks` (default 1024), one-chunk calls, and slices below
`reduce_sum_min_elements` (currently 8192 outer elements) stay serial. These
controls bound compilation/storage and avoid dispatch overhead on small calls;
actual benefit still depends on callback cost and memory bandwidth. Fixed
partitions are deterministic across worker schedules; changing the compile-time
partition may change floating-point rounding and resulting sampling trajectories.

Each child retains its compact values, adjoints, scratch and gradient buffer.
Only the input ranges that child reads are copied per evaluation. Forward and
reverse dispatch separately, so reverse uses the actual incoming adjoint even
inside nonlinear expressions. Shared input gradients merge in fixed child order.
Buffers and threads are reused; kernels and exceptional paths may still allocate.
A failed job drains the team before propagating the first error in chunk order,
and a later fresh gradient can reuse the executor.

Validation and benchmark evidence: [integration record](superpowers/plans/2026-09-18-native-reduce-sum-integration.md).
