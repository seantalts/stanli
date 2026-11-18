# Laplace whitening

Laplace whitening is an experimental transform for models with many
correlated random effects. You must opt in to use it. It works in three
steps:

1. It finds the marginal mode of the random effects.
2. It factors the negative Hessian at that mode with a sparse Cholesky-style
   factor L.
3. It returns a new model whose parameters are z, with
   `q = q_hat + P' L^-T z`.

The sampler then runs on z with an identity metric. Output rows, generated
quantities and initial values still use the original parameters.

The default sampler does not change, and nothing here runs unless you call
it. The first call in a session warns you that the feature is experimental.
Python issues an `ExperimentalWarning`, and R prints a message.

The idea came from #540. The transform was added in #565, and `@latent` in
#564.

## When it helps

In a centred hierarchical model, the posterior over the random effects is
strongly correlated with the hyperparameters. A diagonal metric cannot remove
that correlation. A factor taken at the marginal mode can.

The table below compares the minimum bulk ESS per gradient with the default
sampler. The gradients that the builder uses and the warmup are included. The
runs used macOS arm64 and 2 seeds (#565).

| model | ESS per gradient |
| --- | --- |
| radon hierarchical intercept, centred | 69x higher |
| `crossed_c` (two crossed random intercepts) | 51x higher |
| `gauss_slopes` | 13x higher |
| `spatial_binom_gmrf` | 2.8x higher |
| `bern_glmm` | 1.5x higher |
| `irt_2pl` | 14x lower |

In wall time, `gauss_slopes_c` (606 parameters) went from 106 to 1,257 ESS per
second. The 2,006-parameter version went from 31 to 328. These timings ran on
a loaded machine and still need a rerun on an idle one.

## When it hurts

- Non-centred models. The random effects are already close to independent, so
  there is little to undo and little to gain.
- Models where the Hessian at the marginal mode is not positive definite.
  The builder repairs the negative eigenvalues and warns. The whitened
  coordinates are then only approximately standard normal. `bern_glmm_big`
  needed repair and went from 231 to 37 ESS per second.
- `irt_2pl`, where the table above shows a loss. Run a short default fit
  before you start a long whitened one.

The builder is slow for large models. With 12,006 parameters it took 64 s.
About half of that time went to a dense Hessian, which the builder uses to
find the sparsity pattern.

## Marking the random effects

The builder needs to know which parameters are the random effects u. The rest
are hyperparameters. You can annotate them in the Stan file:

```stan
parameters {
  real mu;
  real<lower=0> sigma;
  @latent vector[J] alpha;
}
```

Or pass `latent=` (Python) or `latent =` (R) with the parameter names. If you
pass a list, it takes precedence over the annotations.

`@latent` is a declaration annotation (stan-dev/stanc3#1439). Stanli's
compiler supports these annotations. Released stanc3 (2.40, which CmdStan
uses) does not support them yet, so CmdStan rejects a model that uses one. If
the model must also compile with CmdStan, pass `latent=` instead.

Annotations on transformed parameters and generated quantities have no
effect. If there are no annotations and no list, the call fails with a
message that names both fixes.

## Sampling

Python (needs numpy and scipy, which the package does not require):

```python
model = stanli.Model(stan_file="model.stan", data="data.json")
whitened, report = stanli.laplace_whiten(model, seed=1)
fit = whitened.sample(chains=4, warmup=200, samples=1000, seed=1,
                      metric="unit_e", adapt_metric=False)
```

R (needs the Matrix package):

```r
m <- stanli_model(file = "model.stan", data = data)
out <- laplace_whiten(m, seed = 1)
fit <- sample_model(out$model, chains = 4, warmup = 200, samples = 1000,
                    metric = "unit_e", adapt_metric = FALSE, seed = 1)
```

Use `unit_e` and adapt only the step size. The factor already does the job of
the metric. The benchmark above used a warmup of 200.

The returned model also works with `log_prob` and `unconstrain`. In R, it
keeps its whitening when `sample_model` rebuilds it for a new seed.

## Reading the report

`report` holds:

- `n`, `n_latent` and `n_theta`.
- `q_hat` and `theta_hat`, the marginal mode.
- `L`, the factor, and `nnz_L`, its fill.
- `gradients`, the number of gradients spent on building. It has a count for
  the pattern Hessian, the search for the marginal mode, the final Hessian,
  and a total.

The benchmark counts these gradients as part of the whitened run. On small
models they are a visible fraction of the run.

`n_clipped` is the number of eigenvalues that the builder repaired, and
`min_eigenvalue` and `max_eigenvalue` come with it. Zero is the good case. A
positive count comes with a warning. It means that the Hessian at the mode was
not positive definite, and the factor describes a nearby matrix. Treat a
large share of repaired eigenvalues as a sign that whitening will not help
this model.

`repair="abs_dense"` does the same repair with dense linear algebra, which is
slow. `repair="clip"` is a cruder repair.

## After a fit

Whitening changes the geometry that the sampler sees. The target distribution
stays the same, so the posterior should agree with a default fit. Run one
default fit on the same data, and compare the posterior means and standard
deviations. The whitened fit has not converged if a mean differs by more than
about 0.3 posterior standard deviations, or if R-hat is above 1.05.

`tools/laplace_whiten_acceptance.py` runs this comparison on six models and
scores them by minimum bulk ESS per gradient.
