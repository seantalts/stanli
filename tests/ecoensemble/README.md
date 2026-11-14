# Model from EcoEnsemble

[EcoEnsemble](https://cran.r-project.org/package=EcoEnsemble) combines the
outputs of several ecosystem models (simulators), and optionally of the
models driving them (drivers), into one estimate of the true state, following
Spence et al. (2018), "A general framework for combining ecosystem models".
The ensemble is a dynamic linear model, and the Stan models evaluate its
likelihood with a Kalman filter written in Stan.

The corpus has `ensemble_model_hierarchical_withdrivers.stan`, the Kalman
filter model for simulators with drivers. It reaches things the rest of the
corpus has only in synthetic models: a `cov_matrix` parameter `SIGMA_t` with
an `inv_wishart` prior, a `cov_matrix` data field, `eigenvalues_sym` and
`eigenvectors_sym` in transformed data, a `while` loop in a function called
from transformed data, and arrays of `corr_matrix` parameters.

The model comes from the CRAN release 1.2.0:

- URL `https://cran.r-project.org/src/contrib/EcoEnsemble_1.2.0.tar.gz`,
  SHA-256 `769b611f50055b169864df8038f67a71973da99e1160cae32539b0f3db4b37e2`
- `inst/stan/ensemble_model_hierarchical_withdrivers.stan`, SHA-256
  `a5e83a3989c04909d255cbb71f4a06297dfebe3523702d99a9e7139a08e33c58`
- By Michael A. Spence, James A. Martindale, Michael J. Thomson, Thomas I. J.
  Bartos and Luke E. K. Broadbent, license GPL (>= 3), text in
  [`licenses/GPL-3`](licenses/GPL-3)

The file has no `#include`, and the checked-in copy is the file as published
in the tarball. The installed package holds a different copy, because
`rstantools::rstan_config()` rewrites every file in `inst/stan/` into stanc's
canonical format at install time.

There are two entries, which share the model and the data set and differ in
the prior on the discrepancy correlations:

| Entry | Size | `form_prior_ind_st` | `form_prior_ind_lt` | `form_prior_sha_st` | Feature |
| --- | --- | --- | --- | --- | --- |
| `ecoensemble_hierarchical_withdrivers` | N 2, time 10, M 2, MM 3, 63 parameters | 3, hierarchical beta | 0, LKJ | 0, LKJ | the package defaults, `EnsemblePrior(2)` |
| `ecoensemble_hierarchical_withdrivers_conj` | same | 4, conjugate beta | 1, inverse Wishart | 2, beta | `lbeta` of parameters, `inv_wishart` on `corr_matrix` parameters, `beta_lpdf` of `atan` |

Form 4 is the only path in the model to `lbeta`, through `beta_conj_prior`.
The second entry's long-term and shared short-term priors are those of
`priors2` in the package's test file.

Both entries use 2 species, 2 simulators by 3 drivers, 10 years of model
output and 8 years of observations, 63 unconstrained parameters. The
package's own data sets, the North Sea outputs such as `SSB_ewe`, have no
driver dimension, so they cannot feed this model. The drivers data set is the
one defined in the package's `tests/testthat/test-post_process_withdrivers.R`,
copied into the generator. Each `.json` file is the `@stan_input` slot of the
`EnsembleData(..., drivers = TRUE)` object, which is the list that
`fit_ensemble_model(..., drivers = TRUE, sampler = "kalman")` passes to rstan,
restricted to the data block's fields.

[`tools/gen_ecoensemble_data.R`](../../tools/gen_ecoensemble_data.R) writes
the four files from the installed package and the CRAN tarball, after
checking both hashes above. Rerunning

    Rscript tools/gen_ecoensemble_data.R tests/ecoensemble [EcoEnsemble_1.2.0.tar.gz]

reproduces them byte for byte. Without the second argument the script
downloads the tarball from CRAN.

The package's `_explicit` variants with drivers,
`ensemble_model_withdrivers_explicit.stan` and
`ensemble_model_hierarchical_withdrivers_explicit.stan`, are not in the
corpus: as published they declare a function returning `int[]`, which
current stanc rejects ("Ill-formed type. Expected an unsized type in function
signature").
