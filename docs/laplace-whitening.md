# Laplace whitening

Laplace whitening is an experimental, opt-in transform for models with many
correlated random effects. It finds the marginal mode of the random effects,
factors the negative Hessian there with a sparse Cholesky-style factor L, and
returns a new model whose parameters are z, with `q = q_hat + P' L^-T z`. The
sampler then runs on z with an identity metric. Output rows, generated
quantities and initial values still use the original parameters. The default
sampler does not change, and nothing here runs unless you call it.

The idea is from #540. The transform is #565, and `@latent` is #564.

## When it helps

On a centred hierarchical model the posterior over the random effects is
strongly correlated with the hyperparameters, and a diagonal metric cannot
undo that. A factor taken at the marginal mode can. Minimum bulk ESS per
gradient against the default sampler, builder gradients and warmup included
(macOS arm64, 2 seeds, #565):

| model | ESS per gradient |
| --- | --- |
| radon hierarchical intercept, centred | 69x higher |
| `crossed_c` (two crossed random intercepts) | 51x higher |
| `gauss_slopes` | 13x higher |
| `spatial_binom_gmrf` | 2.8x higher |
| `bern_glmm` | 1.5x higher |
| `irt_2pl` | 14x lower |

In wall time, `gauss_slopes_c` (606 parameters) went from 106 to 1,257 ESS per
second and the 2,006-parameter version from 31 to 328. Those timings ran on a
loaded machine and need an idle rerun.

## When it hurts

- Non-centred models. The random effects are already close to independent, so
  there is little to undo and little to gain.
- Models where the Hessian at the marginal mode is not positive definite.
  The builder repairs the negative eigenvalues and warns. The whitened
  coordinates are then only approximately standard normal. `bern_glmm_big`
  needed repair and went from 231 to 37 ESS per second.
- `irt_2pl`, where the table above shows a loss. Check a short default fit
  before committing to a long whitened one.

Large models build slowly. At 12,006 parameters the builder took 64 s, about
half of it a dense Hessian used to find the sparsity pattern.

## Marking the random effects

The builder needs to know which parameters are the random effects u. The rest
are hyperparameters. Either annotate them in the Stan file:

```stan
parameters {
  real mu;
  real<lower=0> sigma;
  @latent vector[J] alpha;
}
```

or pass `latent=` (Python) or `latent =` (R) with parameter names. A list
wins over annotations. `@latent` comes from declaration annotations
(stan-dev/stanc3#1439), which stanli's compiler includes but released stanc3
(2.40, used by CmdStan) does not yet: CmdStan rejects a model that uses one.
Pass `latent=` for a model that must also compile with CmdStan. Annotations on
transformed parameters and generated quantities have no effect. With no annotations and no list the
call fails with a message naming both fixes.

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

Use `unit_e` with step-size-only adaptation, since the factor already plays
the part of the metric. The benchmark above used a warmup of 200.

The returned model works with `log_prob` and `unconstrain` too, and in R it
keeps its whitening when `sample_model` rebuilds it for a new seed.

## Reading the report

`report` holds `n`, `n_latent` and `n_theta`, the marginal mode `q_hat` and
`theta_hat`, the factor `L` and its fill `nnz_L`, and `gradients`, the count
spent building (the pattern Hessian, the marginal mode search, the final
Hessian, and a total). The benchmark charges these to the whitened run. On
small models they are a visible fraction of it.

`n_clipped` is the number of eigenvalues repaired, with `min_eigenvalue` and
`max_eigenvalue`. Zero is the good case. A positive count comes with a
warning: the Hessian at the mode was not positive definite, and the factor
describes a nearby matrix. Treat a large share of repaired eigenvalues as a
sign that whitening will not help this model. `repair="abs_dense"` does the
same repair with dense linear algebra, which is slow, and `repair="clip"` is
cruder.

## After a fit

Whitening changes the geometry, not the target, so the posterior should agree
with a default fit. Run one default fit on the same data and compare posterior
means and standard deviations. A mean more than about 0.3 posterior standard
deviations away, or R-hat above 1.05, means the whitened fit has not
converged.

`tools/laplace_whiten_acceptance.py` runs this comparison on six models and
scores them by minimum bulk ESS per gradient.
