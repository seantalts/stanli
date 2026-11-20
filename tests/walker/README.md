# Models from walker

[walker](https://cran.r-project.org/package=walker) fits Bayesian regression
models whose coefficients follow random walks. The corpus has two of its Stan
programs for the same data:

| Entry | Model | What it does |
| --- | --- | --- |
| `walker_rw1_model` | `rw1_model.stan` | a Kalman filter written in Stan functions that marginalizes the random walk coefficients, with generated quantities that draw them |
| `walker_rw1_model_naive` | `rw1_model_naive.stan` | the same model with the coefficients as parameters, so the number of parameters grows with the number of observations |

The first is a hand-written filter loop whose state depends on the parameters.
The second has the coefficients as a 3 by 100 matrix of parameters.

The models come from the CRAN release 1.0.10:

- URL `https://cran.r-project.org/src/contrib/walker_1.0.10.tar.gz`, SHA-256
  `2b220cfdccb4ac36432b33dd72fc9a8f50a0468dc80be972e9216de87dd4b78c`
- `inst/stan/rw1_model.stan` and `inst/stan/rw1_model_naive.stan`
- By Jouni Helske, license GPL (>= 3), text in [`licenses/GPL-3`](licenses/GPL-3)
  and [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

Neither file has an `#include`, and the checked-in `.stan` files are the files
as published in the tarball.

## Data

The package has no data set. The data are the simulation in the first example
of `walker_rw1()` (`set.seed(123)`, 100 observations, an intercept and two
covariates with random walk coefficients), put into the list that
`walker_rw1()` builds for it. The details are in the header of
[`tools/gen_walker_data.R`](../../tools/gen_walker_data.R), which reads the
unpacked tarball and reproduces the `.stan` and `.json` files byte for byte.

## References

The references are CmdStan 2.40.0 values at the three shared points, recorded
as for the other imported collections (see
[`tests/examplemodels/README.md`](../examplemodels/README.md)). Both models
match within the 1e-9 gate at every point, in default and fast mode; the
largest distance is 10 ULP.
