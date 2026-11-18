# Fast mode

Fast mode is an opt-in setting for each model. It lets stanli compute the log
density and gradient in a different order and form from CmdStan. This makes
many models faster.

In default mode, stanli matches CmdStan to within a few units in the last
place. In fast mode, stanli gives up that match to run faster. On the test
corpus, fast-mode results differ from default-mode results by rounding only.
The largest difference is about 1e-13 of the largest gradient entry.

## Using it

Python:

```python
model = stanli.Model(stan_file="model.stan", data="data.json", fast_math=True)
fit = model.sample(seed=1, chains=4)
```

R:

```r
m <- stanli_model(file = "model.stan", data = data, fast_math = TRUE)
fit <- sample_model(m, chains = 4, seed = 1)
```

Browser and Node:

```js
const fit = await sample({ code, data, fastMath: true });
```

Command line: `stanli_run model.stan data.json --fast-math`.

You might compile Stan to MIR yourself (`stan_to_mir` in Python, `compile()`
in JavaScript) and then build the model from that MIR. If so, pass the same
setting to both steps. Fast mode changes the compiled MIR as well as the
runtime.

## What it changes

If a likelihood term repeats the same rows of data, stanli evaluates it once
for each distinct row.

A `normal` or `lognormal` term with a data variate becomes a set of statistics
for each group. If its location is a linear predictor, it becomes one
quadratic form instead. This includes `normal_id_glm`.

`exponential`, `gamma`, `inv_gamma`, `beta`, `poisson`, `bernoulli` and
`binomial` terms become a count and two sums for each group. This also covers
their log and logit forms and their GLMs.

The compiler runs stanc3's partial evaluation. This rewrites linear
predictors such as `a + b * x` as `fma(b, x, a)`. CmdStan's default `-O0`
does not do this.

If the same operation on parameters appears several times, stanli computes it
once and sums its gradients before the backward pass. Loops that read the same
parameter at several positions are fused into vector operations.

## How much faster

These results come from the shared test corpus on Apple M3 Ultra.
Run: <!--gen:fast_runs-->2026-11-13, Stanli source `b4247512`, default and fast mode from the same binaries<!--/gen-->.

| Measure | Result |
| --- | ---: |
| Models with timings | <!--gen:fast_population-->346<!--/gen--> |
| Geometric-mean speedup over default mode | <!--gen:fast_geomean-->1.90x<!--/gen--> |
| Median speedup over default mode | <!--gen:fast_median-->1.03x<!--/gen--> |
| Models at least 2x faster than default mode | <!--gen:fast_ge_two-->82<!--/gen--> |
| Models at least 10x faster | <!--gen:fast_ge_ten-->40<!--/gen--> |
| Models at least 100x faster | <!--gen:fast_ge_hundred-->8<!--/gen--> |
| Largest speedups | <!--gen:fast_top_two-->radon_pooled 551x, nes 253x<!--/gen--> |
| Geometric-mean speedup over CmdStan, default mode | <!--gen:fast_cmdstan_default-->2.25x<!--/gen--> |
| Geometric-mean speedup over CmdStan, fast mode | <!--gen:fast_cmdstan_fast-->4.27x<!--/gen--> |
| Models where fast mode is faster than CmdStan | <!--gen:fast_beats_fast-->338<!--/gen--> |

Most models speed up only a little. Models with repeated observations or
repeated work speed up a lot.
In fast mode, <!--gen:fast_slowest-->the slowest model, `s2_multinomial`, is 1.05x slower.<!--/gen-->

Details and the table for each model are in
[Benchmarks](benchmarks.md#fast-mode).

## How accurate

We check fast mode against an 80-digit reference, computed from each model's
MIR. The check uses the same three points as the CmdStan comparison
([report](../notes/performance/2026-10-09-fast-mode-hp-accuracy.md)).

On 328 models, CmdStan and default mode both agree with the reference. On
these models, fast mode is as accurate as default mode for the log density
and the gradient. This holds at the median, the 90th percentile and the
maximum. One model has a less accurate gradient (3.7e-14 against 3.5e-15).
The rewrite for repeated observations makes 12 log densities or gradients
more accurate, by up to 500x.

We also sampled 120 models in both modes. The posteriors from the two modes
agree with each other, and with posteriordb's reference posteriors, as
closely as two default runs with different seeds do. The effective sample
size per gradient is the same in both modes
([record](../notes/performance/2026-10-09-fast-mode-sampling-check.md)).

## What it does not promise

The same seed gives different draws in fast mode and in default mode.
Rounding differences change the sampler's trajectories. The chains differ,
but they sample the same posterior.

The ULP limits of the CmdStan comparison do not apply to fast mode.
[TESTING.md](../TESTING.md#fast-mode) describes the check that fast mode has
instead.

`bridgestan_model` does not accept the option yet.
