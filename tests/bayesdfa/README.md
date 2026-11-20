# Model from bayesdfa

[bayesdfa](https://cran.r-project.org/package=bayesdfa) fits Bayesian dynamic
factor analysis models to multivariate time series: a few latent trends
(random walks, AR(1) processes, B-splines or Gaussian processes) that the
observed series load on. Its one Stan program, `dfa.stan`, has 681 lines and
a data block of about 70 fields that choose the observation family, the trend
model, covariates and missing-data handling. The corpus has it with two data
sets that take different branches:

| Entry | Data | Branch |
| --- | --- | --- |
| `bayesdfa_dfa` | 3 series of 20 years, 1 trend | the default trend model (`est_rw = 1`), Gaussian observations |
| `bayesdfa_dfa_gp` | the same series, 1 trend | `trend_model = "gp"` with 5 knots: `est_gp = 1`, which calls `gp_exp_quad_cov` on the knot locations |

The model comes from the CRAN release 1.3.4:

- URL `https://cran.r-project.org/src/contrib/bayesdfa_1.3.4.tar.gz`, SHA-256
  `16acf3a3442a5aeaf348802a89ff21353ecc385026393e632dee4d6d77372305`
- `inst/stan/dfa.stan`, SHA-256
  `4b553e775035a3965f9500391355ba9d10a385a0d9e9d9e8a15590e0eb4fc363`
- By Eric J. Ward, Sean C. Anderson, Luis A. Damiano, Michael J. Malick and
  Philina A. English, with contributions from Mary E. Hunsicker, Mike A.
  Litzow, Mark D. Scheuerell, Elizabeth E. Holmes and others, license GPL
  (>= 3), text in [`licenses/GPL-3`](licenses/GPL-3) and
  [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

The file has no `#include`. Each entry has its own copy of it, as published in
the tarball, because the corpus pairs one `.stan` file with one `.json` file.

## Data

The package ships no data set. Both entries are the Stan data that the
package's own `fit_dfa()` builds for its examples, on the data of
`set.seed(42); sim_dfa(num_trends = 1, num_years = 20, num_ts = 3)`.
[`tools/gen_bayesdfa_data.R`](../../tools/gen_bayesdfa_data.R) sources the
package's R files from the unpacked tarball and calls `fit_dfa(...,
estimation = "none")`, which returns the argument list for `rstan::sampling()`
without sampling. It needs `dplyr`, `reshape2`, `mgcv` and `loo` and no
compiler. Rerunning it reproduces the four files byte for byte.

## References

The references are CmdStan 2.40.0 values at the three shared points, recorded
as for the other imported collections (see
[`tests/examplemodels/README.md`](../examplemodels/README.md)).

`bayesdfa_dfa` matches within the 1e-9 gate at every point, in default and fast
mode; the largest distance is 2,949 ULP (2.1e-14 relative).

`bayesdfa_dfa_gp` is a gap in stanli: it refuses the model with
`unsupported function gp_exp_quad_cov`, at all three points, while CmdStan
evaluates it. The model is in `KNOWN_GAPS` in `tools/verify_refs.py`, and the
references hold CmdStan's 14 values at each point, so the replay starts
enforcing them when stanli supports the function and the entry is deleted.
