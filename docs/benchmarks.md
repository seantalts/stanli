# Benchmarks

Stanli gets you from Stan source to posterior draws without a per-model C++
build. During sampling, it reuses a prepared autodiff graph and batches
independent work. This can shorten the first fit and repeated gradient
evaluations; the gain depends on the model.

The current comparison covers **319 application models** using one build and
one measurement protocol. **Current sweep: results pending.**
The table will report the completed experiment, including failures and timeouts.

<a id="benchmark-method"></a>

## How we measure

**Gradient speedup is CmdStan time / Stanli time:** above 1x favors Stanli.
Both engines evaluate the same log density and full gradient at a shared
unconstrained point. After warmup, six alternating pairs give a median
speedup and median absolute deviation (MAD). Every accepted pair must pass
its numerical comparison.

**Complete sampling includes startup, preparation, adaptation, generated
quantities and CSV output.** Each engine runs four seeds with 1,000 warmup
iterations and 1,000 saved draws per seed. The table reports median CLI time;
CmdStan model compilation is measured separately. This is a fixed sampling
budget, not time to equal effective sample size. Diagnostic warnings and
incomplete runs remain visible.

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
here. Missing timings will include their failure or timeout reason.

<details>
<summary>Show all 319 models, timings, and diagnostic notes</summary>

<!--gen:benchmark_catalog-->
Current corpus sweep pending. Timings will be published after all selected
models have a recorded outcome and sampling diagnostics have been processed.
<!--/gen-->

</details>

## Appendix

The [benchmark appendix](benchmark-appendix.md) specifies this experiment's
build identity, phase boundaries, numerical checks, sampling diagnostics,
retained artifacts and reproduction commands.
