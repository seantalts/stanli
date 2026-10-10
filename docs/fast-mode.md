# Fast mode

Fast mode is an opt-in setting per model. It lets stanli compute the log
density and gradient in a different order and form from CmdStan, which
makes many models faster. By default stanli matches CmdStan's results to
within a few units in the last place; fast mode gives that up for speed.
Its results differ from default mode only by rounding, about 1e-13 of the
largest gradient entry at most on the test corpus.

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

If you compile Stan to MIR yourself (`stan_to_mir` in Python, `compile()` in
JavaScript) and build the model from that MIR, pass the same setting to both
steps. Fast mode changes the compiled MIR as well as the runtime.

## What it changes

- **Repeated observations.** A likelihood term whose rows repeat over the
  data is evaluated once per distinct row. A `normal` or `lognormal` term
  with a data variate becomes per-group statistics, or one quadratic form
  when its location is a linear predictor (`normal_id_glm` included).
  `exponential`, `gamma`, `inv_gamma`, `beta`, `poisson`, `bernoulli` and
  `binomial` terms (with their log and logit forms and GLMs) become a count
  and two sums per group. `STANLI_NO_COLLAPSE=1` turns this part off.
- **Fused multiply-adds.** The compiler runs stanc3's partial evaluation,
  which rewrites linear predictors such as `a + b * x` into `fma(b, x, a)`.
  CmdStan's default `-O0` does not.
- **Shared work.** Repeated operations on parameters are computed once and
  their gradients summed before the backward pass, and loops that read the
  same parameter at several positions are fused into vector operations.

## How much faster

On the shared test corpus (336 models with timings, Apple M3 Ultra,
2026-10-10, default and fast mode from the same build):

- Fast mode is 2.03x faster than default mode in geometric mean. The median
  model is 1.06x faster: most models gain little, and models with repeated
  observations or repeated work gain a lot. 87 models are at least 2x
  faster, 43 at least 10x, and 7 at least 100x (radon_pooled 493x, nes
  251x).
- Against CmdStan, stanli's geometric-mean speedup goes from 1.95x in
  default mode to 3.95x in fast mode, and fast mode is faster than CmdStan
  on 321 of the 336 models.
- No model is measurably slower in fast mode. The few that timed up to 6%
  slower were within noise when rechecked in CPU cycles.

Details and the per-model table are in [Benchmarks](benchmarks.md#fast-mode).

## How accurate

Fast mode is checked against an 80-digit reference computed from each
model's MIR, at the same three points the CmdStan comparison uses
([report](../notes/performance/2026-10-09-fast-mode-hp-accuracy.md)). Over
the 328 models where CmdStan and default mode both agree with the reference,
fast mode is as accurate as default mode at the median, the 90th percentile
and the maximum, for the log density and the gradient. One model's gradient
is less accurate (3.7e-14 against 3.5e-15). The repeated-observation work
makes 12 log densities or gradients more accurate, up to 500x.

Sampling 120 models in both modes gave posteriors that agree with each
other and with posteriordb's reference posteriors as closely as two default
runs with different seeds do, and the same effective sample size per
gradient ([record](../notes/performance/2026-10-09-fast-mode-sampling-check.md)).

## What it does not promise

- The same draws as default mode for the same seed. Rounding differences
  change the trajectories, so chains differ while the posterior does not.
- Agreement with CmdStan to the last bits. Fast mode is not held to the
  CmdStan comparison's ULP limits; its own check is described in
  [TESTING.md](../TESTING.md#fast-mode).
- `bridgestan_model` does not take the option yet.
