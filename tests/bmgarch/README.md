# Models from bmgarch

[bmgarch](https://cran.r-project.org/package=bmgarch) fits Bayesian
multivariate GARCH models: constant (CCC) and dynamic (DCC) conditional
correlation, and BEKK, with a constant, ARMA(1,1) or VAR(1) mean structure and
normal or Student-t errors. The Stan programs hold the time-varying covariance
matrix `H[t]` as an array of `cov_matrix` transformed parameters built by a
recursion over time, with GARCH weights that come from simplexes through
user-defined functions. The corpus has two of them:

| Entry | Model | Mean structure | Data |
| --- | --- | --- | --- |
| `bmgarch_ccc` | `CCCMGARCH.stan` | constant | `panas`: 200 observations of two series |
| `bmgarch_bekk_arma` | `BEKKMGARCH.stan` | ARMA(1,1) | `panas` |

Both use GARCH(1,1) (`P = 1`, `Q = 1`) and Student-t errors, so they reach
`multi_student_t`, `cov_matrix` and `corr_matrix` parameters, `simplex` arrays
and the package's helper functions.

The models come from the CRAN release 2.1.0:

- URL `https://cran.r-project.org/src/contrib/bmgarch_2.1.0.tar.gz`, SHA-256
  `e883500018f13bd171974c2d36364f9aa6abf254f5438fba7f796d5ba8272a70`
- `inst/stan/CCCMGARCH.stan` and `inst/stan/BEKKMGARCH.stan`
- By Philippe Rast and Stephen Martin, license GPL (>= 3), text in
  [`licenses/GPL-3`](licenses/GPL-3) and
  [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

## Changes to the source

Each file is assembled from `#include /path` lines that name files under
`inst/stan`. The corpus pairs one self-contained `.stan` file with one `.json`
file, so each `#include` line is replaced by the text of the file it names,
recursively and unchanged. Nothing else is edited.

## Data

`bmgarch()` builds the Stan data with the internal function `standat()` in
`R/bmgarch.R`. [`tools/gen_bmgarch_data.R`](../../tools/gen_bmgarch_data.R)
evaluates that function from the unpacked tarball and calls it as the
package's example does, on `data(panas)` with the defaults (no standardizing, no
predictor `xC`). It also writes the `.stan` files. Rerunning it reproduces all
four files byte for byte.

## References

The references are CmdStan 2.40.0 values at the three shared points, recorded
as for the other imported collections (see
[`tests/examplemodels/README.md`](../examplemodels/README.md)). Both models
match within the 1e-9 gate at every point, in default and fast mode; the
largest distance is 70 ULP.

Both models are in `KNOWN_GAPS` in `tools/verify_refs.py`: their generated
quantities call `multi_student_t_rng`, which `write_array` lacks, so the
generated quantities fall back to the MIR interpreter, which throws
(`unsupported function multi_student_t_rng`). The entries excuse only the
`write_array`: the replay still compares the log density and gradient at
every point. Neither has a recorded `write_array` row.

Their benchmark points stay fixed for the same reason: the NUTS run reads
every column of the generated quantities.
