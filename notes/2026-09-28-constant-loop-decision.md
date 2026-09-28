# Large callback loops: preparation improves, execution regresses

Keep the current production unrolling policy for now. Retaining a large loop
in the existing register engine removes substantial preparation and code
storage, but this experiment roughly doubles warm gradient and inference time.
That would introduce the kind of performance cliff we are trying to prevent.

This is the next major design decision after the bounded coverage patches.
The user authorized continuation until completion or a major design decision.
No production loop-policy change or new derivative machinery is included here.

## Experiment and correctness

The hypothesis was that using existing register back edges could eliminate
large constant-loop expansion without a new engine. The competing hypothesis
was that lost derivative optimizations and loop overhead would outweigh the
smaller code. Both are observable without implementing a new lowering policy.

`ode_constant_loop.stan` sums a fixed number of terms in an ODE callback.
`ode_retained_loop.stan` obtains its trip count from the current state. For
the positive state reached by this experiment, both perform the same number
of additions in the same order. The latter has one extra branch to select the
trip count; this is a representation probe, not an exact same-source compiler
ablation. Both use the current register compiler and unchanged solver kernels.

The fixtures constrain the rate to (0, 1), start the state at 1, and integrate
to time 0.1. The state stays positive at the tested points. Independent CmdStan
drivers at three points and sizes 8, 128, and 2,048 compare 72 density, gradient,
and output values at 0 ULP. All timing checksums match between representations.
The default-size recordings are permanent regression fixtures.

Measurements use Release/AppleClang 21 on Darwin arm64 at b6963d5a, six
alternating pairs in fresh processes, without simultaneous builds or tests.
The same library runs both sources. Source compilation, preparation, first
evaluation, warm evaluation, output rows, short inference, and process RSS are
recorded separately. Short inference is 100 warmup plus 100 retained draws
and their output rows. The 8-iteration case is the small-workload canary;
there is no production change requiring an unrelated performance comparison.

## Measured medians

| Iterations | Representation | Preparation µs | Native gradient µs | Inference ms | Peak process MB |
| ---: | --- | ---: | ---: | ---: | ---: |
| 8 | Expanded | 256.9 | 1.58 | 1.79 | 28.27 |
| 8 | Retained | 247.1 | 3.12 | 3.01 | 28.49 |
| 128 | Expanded | 546.3 | 13.99 | 11.11 | 28.46 |
| 128 | Retained | 239.6 | 31.07 | 21.67 | 28.49 |
| 2,048 | Expanded | 5,296.3 | 207.08 | 165.32 | 30.96 |
| 2,048 | Retained | 243.3 | 441.45 | 342.97 | 29.05 |

At 2,048 iterations, compact code saves about 5 ms in preparation but adds
about 234 µs per gradient. Roughly 22 gradients consume that preparation
saving, before considering other phases. Whole-process peak RSS is lower,
but it does not measure retained model memory or derivative history alone.

At 128 iterations, expansion uses 517 registers and 642 instructions; retaining
the loop uses 22 registers and 29 instructions. At 2,048, expansion grows to
8,197 registers and 10,242 instructions while the retained program stays the
same size. The compact program still stores differentiation work during
execution; small prepared code does not imply constant-memory differentiation.

The execution manifest confirms that the expanded callback selects direct RK
sensitivities. The retained callback loses that path because `JZ` is outside
its exact-forward whitelist. Independently, generated register reverse refuses
back edges because its current per-block flags represent at most one execution
of each block.

A second six-pair native-gradient experiment disables direct RK for both
sources. At 128 iterations, expanded/retained gradients are 19.72/30.30 µs;
at 2,048 they are 277.48/439.42 µs. Losing direct RK therefore explains only
part of the difference. Loop/control overhead and the changed register reuse
also matter; this experiment does not isolate those individual costs.

## Recommended next experiment

Test whether a fixed-shape callback can reuse the existing structured-loop
engine's iteration history and kernel backwards. Give the callback explicit
inputs, outputs, and a weighted reverse operation. Preserve the precise Stan
forward arithmetic and adaptive solver behavior. Merely adding a jump opcode
to the direct-RK whitelist would not provide a correct derivative.

The serious alternative is extending generated register reverse with
per-iteration value versions and control history. It avoids translating the
body into graph operations, but introduces loop-history machinery alongside
the structured engine's existing implementation. Neither alternative is
implemented or measured here. Prefer testing reuse first, while keeping the
alternative available if adapter overhead dominates.

Bound the initial experiment to scalar counted loops with fixed storage.
Test zero/one/many trips, overwritten carry values, changed trip counts,
branches, repeated evaluations, and weighted input derivatives against Stan.
Compare expanded, retained-register, and structured-callback paths at the
three sizes above. Measure preparation, warm gradients, inference, and the
actual retained history, including bytes per iteration. Do not change default
admission until compact code has a useful end-to-end performance case. Then
extend to fixed-shape gathers/writes and more general control separately.

This is work within the existing engines, not native instruction generation.
It is deferred at this design checkpoint rather than silently selecting the
currently slower compact route.

Raw samples, dispersion, engine manifests, checksums, the direct-RK ablation,
and external numerical results are in `2026-09-28-constant-loop-performance.json`.
Scripts, per-point oracle output, and logs are in `/tmp/stanli-loop-coverage`.
To reproduce, generate fixture MIR through the normal build, create data JSON
with each `N`, then run `tools/bench_model_phases.py` and `bench_grad --timed`
with 200 ms warmup and 250 ms measurement in alternating fresh processes.
Set `STANLI_NO_ODE_DIRECT_RK=1` for the second experiment.

## Final verification

The preceding runtime-index implementation passed all 304 configured CTests
and the full 329-model recorded corpus. Adding these two loop fixtures changes
no runtime code; their two reference tests and the affected callback-geometry
checks pass (four focused CTests). The final configured suite has 306 tests.
Installed Python tests and installed R package tests also pass with the final
native library. Their logs are retained with the experiment. No Fable review
was requested for these implementation checks.
