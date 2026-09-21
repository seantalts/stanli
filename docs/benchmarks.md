# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling, it reuses a prepared autodiff graph and batches
independent work. This can shorten the first fit and repeated gradient
evaluations; the gain depends on the model.

The current comparison covers **<!--gen:benchmark_models-->319<!--/gen--> application
models**. Across <!--gen:corpus_n_grad-->pending<!--/gen--> accepted gradient
comparisons, the median paired speedup is **<!--gen:corpus_median-->pending<!--/gen-->**.
The table includes every model, including failures and timeouts.

<a id="benchmark-method"></a>

## How we measure

**Gradient speedup is CmdStan time / Stanli time:** above 1x favors Stanli.
Both engines evaluate the same log density and full gradient at a shared
unconstrained point. After warmup, six alternating pairs give a median
speedup and median absolute deviation (MAD). Every accepted pair must pass
its numerical comparison.

**Estimated time = setup + 20,000 × median warm gradient time.** Stanli setup
includes source-to-MIR compilation and lowering/binding; CmdStan setup includes
stanc translation and the ordinary C++ model build. This fixed-work estimate
excludes sampler/output overhead. Full sampling is not run in this benchmark.

See the [experiment details](benchmark-appendix.md) and independent
[numerical checks](../TESTING.md#comparison-with-cmdstan-on-complete-models).

## Why some models are faster, and some slower

**Less setup and repeated bookkeeping.** Stanli avoids compiling each model
to C++, binds data-dependent structure once, and reuses its reverse plan and
working buffers. [How the graph works](how-it-works.md#from-stan-source-to-a-bound-operation-graph)
explains what is prepared once and what each evaluation repeats.

**Independent observations offer more work to batch.** Regressions, GLMs and
IRT models can turn repeated scalar work into vector operations, amortizing
instruction dispatch. See [loop recovery](how-it-works.md#recovering-vector-operations-from-scalar-loops)
and the [optimization details](../runtime/src/OPTIMIZATIONS.md).

**Shared kernels and sequential work leave less overhead to remove.** Dense
linear algebra and ODEs spend substantial time in Stan Math in both engines.
Recurrences limit independent batching, and fallback derivatives can retain
local autodiff costs. These costs can bring results close to parity or make
Stanli slower; the per-model measurements show where that happens.

<a id="full-corpus"></a>

## Full corpus results

Every application fixture uses the same run settings. Language-conformance
fixtures belong to the [numerical corpus](corpus-status.md) and are not timed
here. Missing timings include their failure or timeout reason.

<details>
<summary>Show all <!--gen:benchmark_models-->319<!--/gen--> models, timings, and notes</summary>

<!--gen:benchmark_catalog-->
Current corpus sweep pending. Timings will be published after all selected
models have a recorded outcome.
<!--/gen-->

</details>

## Appendix

The [benchmark appendix](benchmark-appendix.md) specifies this experiment's
build identity, phase boundaries, numerical checks, retained artifacts and
reproduction commands. It also contains one full-corpus gradient table against
CmdStan with stanc3 loop vectorization enabled.
