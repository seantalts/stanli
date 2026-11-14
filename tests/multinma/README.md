# Models from multinma

[multinma](https://cran.r-project.org/package=multinma) fits network
meta-analysis and multilevel network meta-regression models, which compare
several treatments across trials that report individual patient data,
aggregate data, or both. It ships one Stan model per likelihood, built from
shared `#include` files. Random treatment effects are correlated within
multi-arm trials, and when the Cholesky factor of that correlation matrix is
at most 10% nonzero the model stores it in CSR form (`csr_extract_w/v/u` in
transformed data) and applies it with `csr_matrix_times_vector`. Every model
sets its priors through the `void` function `prior_select_lp`, called
unconditionally in the model block.

The models come from the CRAN release 0.9.1,
`https://cran.r-project.org/src/contrib/multinma_0.9.1.tar.gz` (SHA-256
`ac9956cf5ba3da029497a145c130995f424705df0527e4a3abdbc59fc302153e`).
Its author and maintainer is David M. Phillippo (the DESCRIPTION names no
separate copyright holder), license GPL-3, text in
[`licenses/GPL-3`](licenses/GPL-3) and
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md).

| Entry | Model | Example | Data | Parameters | Reached because |
| --- | --- | --- | --- | --- | --- |
| `multinma_smoking_re` | `binomial_1par.stan` | the package's `example_smk_re`: random effects NMA of `smoking` | 24 studies, 50 arms, 4 treatments, aggregate counts | 59 | `RE = 1` with 31 random effects; 4.1% of the Cholesky factor is nonzero, so `RE_sparse = 1` and the CSR path runs |
| `multinma_ndmm_mspline` | `survival_mspline.stan` | the `ndmm_fit_nma` model of the ndmm vignette (`likelihood = "mspline"`), on 20 patients per arm | 5 studies, 3 treatments, 120 IPD and 80 reconstructed aggregate survival times | 62 | cubic M-spline baseline hazard with 7 internal knots (11 spline coefficients per study), the model's own survival functions and the `void` prior functions |

The ndmm network has 4,144 patients. The generator keeps 20 per study and arm
(10 arms), drawn with a fixed seed, from both `ndmm_ipd` and `ndmm_agd`.

## Changes to the source

Each `.stan` file is the package's model with every `#include` line replaced
by the text of the file it names, preceded by `inst/stan/include/license.stan`
(comments only, which rstantools puts at the top of the generated C++).
Nothing else is changed. stanc3's `--print-canonical` output of each
checked-in file matches that of the original compiled with
`--include-paths inst/stan`, apart from the license comment and blank lines.

| `inst/stan/` file | SHA-256 |
| --- | --- |
| `binomial_1par.stan` | `ea0e7bd62576f71b76003201ac5119780452f0f15d0ec9035bdb44b680e683cb` |
| `survival_mspline.stan` | `f909b27f94c643ce4f96b606414805f59f84f69e978936a78d12e2c5912e467c` |
| `include/license.stan` | `b16b48e411b01744dc5fcd13727dc4d09cafa5aa26a76ad1f465929c58235159` |
| `include/prior_select.stan` | `5ab3f613b3e1c7b234fd58782db174b708e9b879be24fd341008340210cd595b` |
| `include/count_nonzero.stan` | `a44a9e191fbef99be4acb33fbbc65d5f2a96f547a1b4dd928b130fa5d3e5a68b` |
| `include/vector_functions.stan` | `95d4ee974f8ba18468786417f49bd0b6d6530eb229fc8826a7de40ae2e22a8ea` |
| `include/data_common.stan` | `3f39102ed8817c0d65672efbc253f6246aa7f25e1544ef2c2506e2c777dfed04` |
| `include/transformed_data_common.stan` | `5aba1e6f25ec3ffe1de6569794d002c221fb8af6a1af45c27f7ec5bfb7ba2f42` |
| `include/parameters_common.stan` | `a794b13f301992b57686cb451e38e4276b2950059b8180d5e43837358da58e83` |
| `include/transformed_parameters_theta.stan` | `3fe806bc6d61f9ac021f92a5116c8f07bd37e0b15925e2f23d300343e20d656a` |
| `include/transformed_parameters_common.stan` | `73f08919c717a646b1bce475f91afe7323680f9b653da9e1b1de5f3376089d8e` |
| `include/model_common.stan` | `1ecc18d8cdc9241b943d34c6915f13b77fb79f33775e199fd56132764adf2b4b` |
| `include/generated_quantities_theta_fitted.stan` | `e7bb3e48d90a3cd54f6f01382078f103e1416305228302f69a08b5dac40b299d` |
| `include/generated_quantities_common.stan` | `4cfe2a26d94c20e3c590ff5c9640efb8fb5f7d0daf5c85ee87f13ea255480a3a` |
| `include/generated_quantities_theta.stan` | `a98b53e06ec95c819011b1ddf87674352c9e396a35de5d2b79c2bd3a4f9553e7` |

## The data

Each `.json` file is the list `nma()` passes to `rstan::sampling()`,
restricted to the data block's fields. The generator defines a
`rstan::sampling` method for `stanmodel` that records the list and stops, so
no sampling runs. Two fields need handling:

- `CHAIN_ID` is not in the list. `nma()` passes `chain_id = 1` and rstan adds
  `CHAIN_ID` from it, so the generator writes `CHAIN_ID = 1`. The model then
  uses `nint_vec[1]`, which is 1 here (no numerical integration).
- `prior_aux_location` and `lscoef_weight` (arrays of vectors) come as R lists
  of vectors; the generator writes one row per array element.

[`tools/gen_multinma_data.R`](../../tools/gen_multinma_data.R) writes all
four files from the installed package. Install multinma 0.9.1 first (it
depends on `copula`, which needs the GSL library), then run

    Rscript tools/gen_multinma_data.R tests/multinma

which reproduces the files byte for byte.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.

`unflattened/` holds `binomial_1par.stan` and the eleven files it includes,
unchanged from the package. `test_capi` compiles it with the include path
`tests/multinma/unflattened` and checks that its log density and gradient
equal those of the flattened `multinma_smoking_re.stan` at one point.
