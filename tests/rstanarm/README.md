# Models from rstanarm

[rstanarm](https://cran.r-project.org/package=rstanarm) fits applied
regression models (GLMs, GLMMs, ordinal regression and others) with
precompiled Stan programs that data flags steer between branches. The four
models here are built from 28 shared `#include` files. Group terms reach the
model as a sparse CSR matrix and go through `csr_matrix_times_vector` on the
random-effect parameters.

The models come from the CRAN release 2.32.2,
`https://cran.r-project.org/src/contrib/rstanarm_2.32.2.tar.gz` (SHA-256
`911647aeb6bdc303a229fb3a1d7d8fa2e8e01a146e0a6a51b6322d41219cd1fc`).
Copyright (C) 2015, 2016 2017 Trustees of Columbia University, license
GPL (>= 3), text in [`licenses/GPL-3`](licenses/GPL-3) and
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md).

There are four entries, one per top-level model:

| Entry | Model | Example | Data | Parameters | Reached because |
| --- | --- | --- | --- | --- | --- |
| `rstanarm_glmer_bernoulli` | `bernoulli.stan` | `stan_glmer(r2 ~ Anger + Gender + btype + (1 \| id) + (1 + Gender \| item), family = binomial)` on the first 10 respondents of `lme4::VerbAgg` | 240 rows (137 zeros, 103 ones), 2 group terms, q = 61 | 71 | `t = 2` and `special_case = 0`, so the linear predictor adds `csr_matrix_times_vector(N[k], q, w, v, u, b)` with 312 and 216 nonzeros; the `decov` prior with a 2x2 covariance |
| `rstanarm_glmer_nb_count` | `count.stan` | `stan_glmer.nb(Days ~ Sex/(Age + Eth*Lrn) + (1 \| Eth:Sex:Lrn), data = MASS::quine)`, from `?stan_glmer.nb` | 146 rows, K = 13, 9 levels | 25 | `family = 7` and `t = 1` with `special_case = 1`, so the model gathers `b[V[i]]` (index arrays from `make_V`) and calls `neg_binomial_2_log_lpmf` outside the GLM function |
| `rstanarm_polr` | `polr.stan` | `stan_polr(tobgp ~ agegp, data = esoph, prior = R2(0.2, "mean"))`, from `?stan_polr` | 88 rows, 4 categories, K = 5 | 9 | the ordinal likelihood written as Stan functions, with `simplex`, `unit_vector` and the R2 prior |
| `rstanarm_glm_gaussian` | `continuous.stan` | `stan_glm(mpg ~ wt + am, data = mtcars)` | 32 rows, K = 2 | 4 | `can_do_OLS = 1`, so transformed data calls `CODOLS` and the model block uses `mvn_ols_lpdf` (a `quad_form` on the precomputed `X'X`) |

The flags in the last column are checked against the generated `.json`
files; the script prints them.

## Changes to the source

Each top-level `.stan` file is written with every `#include` line replaced by
the text of the file it names, recursively, since the corpus pairs one
self-contained `.stan` file with one `.json` file. The `#include` lines are
indented in rstanarm's sources; the whole line is replaced. stanc3's
`--print-canonical` output of each checked-in file is identical to that of the
original compiled with `--include-paths inst/stan`, apart from blank lines.
For `continuous.stan`, which does not compile as shipped, the comparison is
against the original with the one line below replaced.

`continuous.stan` has one more change. It declares

    vector CODOLS(matrix X, vector y); // implemented in C++

and rstanarm supplies the body in `inst/include/CODOLS.hpp`: Eigen's
`CompleteOrthogonalDecomposition` solve, which returns the least-squares
coefficients of `y` on `X` (the minimum-norm solution if `X` is rank
deficient). stanc rejects a declaration without a body, so the checked-in
file replaces that line with

    vector CODOLS(matrix X, vector y) {
      return mdivide_left(qr_thin_R(X), qr_thin_Q(X)' * y);
    }

which is the same least-squares solution when `X` has full column rank. The
generator checks that the design matrix it writes (intercept, `wt`, `am`) has
full rank. The function runs once, in transformed data, when `can_do_OLS` is 1:
gaussian family, identity link, no group terms, no offset, no `prior_PD`,
dense `X`, and more rows than coefficients. CmdStan compiles the same
checked-in file, so the references use the same stand-in.

## Source files

Every file below is read from the installed package and checked against its
SHA-256 before anything is written.

| `inst/stan/` file | SHA-256 |
| --- | --- |
| `bernoulli.stan` | `5c171f5c1dcc9dc378eb4267bdd6c55c16911d4538d4b5f104e6ac0c9667c919` |
| `count.stan` | `adfdad3d384f0b80f807253530dd29f56d7c04e46226072899cb01921de036d9` |
| `polr.stan` | `46a8b47e194af6232c81b482f094fbfd022268fd6f0f1f0ea137920898b7182b` |
| `continuous.stan` | `3c70cca332a1f4b13e5ef0f5fae12e0adfda0bc8a72058a90c9415a1e475c9a4` |
| `include/Columbia_copyright.stan` | `4a6ae68ab0159f6aabd53ed7c64b7fb517e1d11111b71a96f155004acd6ef0da` |
| `include/license.stan` | `38cde8b3b39557351231aca32dabfa416d003fbaee6c5830d9bfbcfe6e83ccbd` |
| `functions/common_functions.stan` | `1990d293a0ef7b73af49b1a0177e62408c490ccdbcee3cf13eaa74a473eb43b9` |
| `functions/bernoulli_likelihoods.stan` | `529bcdd2a46ea50e0f214f43303ceba53de702ebc63f1a6f155edb0a0620ba1d` |
| `functions/count_likelihoods.stan` | `0a1351c83a4d30ad68cb986af79550df62479d10d1abd70114bb343bc064e3ac` |
| `functions/continuous_likelihoods.stan` | `627235a7a9297d1ff4d6b299f9fcdb2bfdca87146a3f34167713c28c2e1e77df` |
| `functions/SSfunctions.stan` | `f967fb67c5af48fc35f912035fc25f16cb4c4071e83f7f576ff25ae8631582a2` |
| `data/NKX.stan` | `ca614536b96362d9fc7ec6e6fe88a8c80fa554ed7842a1a4e32d3ae18615f099` |
| `data/data_glm.stan` | `4746e1956f812533d74c2bb72a1608b73ccb6a8987bb2d95fcf1b78c5441d9e3` |
| `data/data_betareg.stan` | `0c888bc5b175be219d838586e684f771493efe8b5e0056b831f30b3205a5c158` |
| `data/glmer_stuff.stan` | `1eaa4f6655644ba107209b00d94defe7ca75b127bae99c3165e81af475d5df2a` |
| `data/glmer_stuff2.stan` | `8ad958d2cc40cee940d0036503d987cb00cfbd5601ecb8f088eb4ba7bb70f976` |
| `data/hyperparameters.stan` | `48bd9d9a69fd084c1ee171ea0928ff3f9c90a5f5f806f26a446313a9873fce00` |
| `data/weights_offset.stan` | `5efd704c67ce11116f48b1f041edbe80d5c1c410852d76110cbc4f77c1d362a5` |
| `tdata/tdata_glm.stan` | `3bbd705f47de6743f62a8b57d9c6e89c003cc21eb8de9bcf359a46111edd3a31` |
| `tdata/tdata_betareg.stan` | `f64d5d724b99a9171f62d4b74078b4af1db4fecdd8a7fb3926bd80babe95ff04` |
| `parameters/parameters_glm.stan` | `a732967fa35ca445033426563fd09a4132685da86b2239d9c1790f3d06a75353` |
| `parameters/parameters_betareg.stan` | `86acbec3a5d1f81f3a99350bafa87df03b694ea6927e07170b2271e55e587e71` |
| `tparameters/tparameters_glm.stan` | `3a7275b8cb716c0da5fa84b50a27fb04cf32ad7dcad07f205d6ef898c2324428` |
| `tparameters/tparameters_betareg.stan` | `c95bf26a8f1995987bc2fe333ab1fff2610628f948bf723f993f56ace60a4cb1` |
| `model/make_eta.stan` | `1d521b2c34dde6fd672b77e4da0fbb8d0f63a7c60788d9b78c3e9bd364d20a35` |
| `model/make_eta_bern.stan` | `e0199d40afa1069a341fdf8b0ef02819b01121a23fe8103c6132052fdcab7ddc` |
| `model/make_eta_z.stan` | `46a5c0d0b30c80e53f60b286b8ec44fc5a0212664a4e9deab2285fb303ae2c73` |
| `model/eta_add_Zb.stan` | `ff4c756e65b0eea0510fd0b5df4fcd97ba95ea61cd1e258c9a62ad7b287f2f06` |
| `model/eta_no_intercept.stan` | `36be10d242f914908a5fe1e649318c1a4b3fff1777ad70e62e725df3b7ef0305` |
| `model/eta_z_no_intercept.stan` | `28876c9bf6d25afeb1b9fcf09bd1a452085f74fb2029973de8016dbc22f52f04` |
| `model/priors_glm.stan` | `567d0c51bad6da81bcc0d0a3e6a124d5811b50328c1112d6bfc7277f355fdae5` |
| `model/priors_betareg.stan` | `8441ab6c5843f51d78a1e205c54440dfa8551f9db8722b683ceebe1b652fc0dc` |

## The data

Each `.json` file is the list rstanarm passes to `rstan::sampling()`,
restricted to the data block's fields. The generator defines a
`rstan::sampling` method for `stanmodel` that records the list and stops, so
no sampling runs. rstan accepts a data field being absent when its declared
size is 0; `stan_polr` relies on that for `weights` (it passes the weights as
`wt`), and the generator writes that field as `[]`.

[`tools/gen_rstanarm_data.R`](../../tools/gen_rstanarm_data.R) writes all
eight files from the installed package. Install rstanarm 2.32.2 first (a
source install compiles its eight Stan models, which takes several minutes),
then run

    Rscript tools/gen_rstanarm_data.R tests/rstanarm

which reproduces the files byte for byte.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.
