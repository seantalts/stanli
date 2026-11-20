# Models from stan-dev/example-models

[stan-dev/example-models](https://github.com/stan-dev/example-models) is the
Stan project's collection of worked examples: the programs of Gelman and
Hill's regression book, Kéry and Schaub's population analysis book, the BUGS
examples, and case-study notebooks. This directory has 23 of them. They fill
gaps that the other collections leave: `gaussian_dlm_obs`, `hmm_marginal`,
`sum_to_zero_vector`, `offset` and `multiplier`, capture-recapture models
other than posteriordb's, state-space and volatility models, item response
models from a source other than posteriordb, and a regression whose branch
depends on a parameter.

The programs are the files of commit
`e5b7d9e2e9ecc375805c7e49e4a4d4c1882b5e3b` (2025-04-30), copied unchanged. At
that commit every file compiles with the pinned stanc, so none needed the
array-syntax update that older Stan programs need. No program was edited.
Their source hashes are below. The replay fails with `INPUT_HASH_FAIL` if a
file differs from the hash recorded with its references.

## License

The repository has no `LICENSE` file at its root. Its `README.md` says that all
examples are under the new BSD license unless the directory of the model
says otherwise; that paragraph is in
[`licenses/example-models-README-licensing`](licenses/example-models-README-licensing).
Two directories that this collection uses carry their own files, both
BSD 3-Clause:

- `BPA/` (Kéry and Schaub), Copyright (c) 2016 Hiroki Itô:
  [`licenses/BSD-3-Clause-BPA`](licenses/BSD-3-Clause-BPA). It covers
  `cjs_cov_raneff`, `cjs_mnl_ran`, `mr_ss`, `ms` and `ssm`.
- `jupyter/sum-to-zero/`, Copyright (c) 2018-2022 Stan Development Team and
  their assignees: [`licenses/BSD-3-Clause-stan-dev-sum-to-zero`](licenses/BSD-3-Clause-stan-dev-sum-to-zero).
  Its file also licenses the notebook text under CC BY-ND 4.0; only the code
  is used here, so only the BSD part is copied.

The data of the two `dlm_fx_` models are Tusell's exchange rate series from
"Kalman Filtering in R" (Journal of Statistical Software 39(2)), as the
upstream `misc/dlm/README.md` says. `dlm_nile` is the annual Nile flow series
(100 values), which upstream ships as `nile.data.json`.

## Models

| Entry | Upstream program | Data | Data size | What it reaches |
| --- | --- | --- | ---: | --- |
| `examplemodels_arch1` | `misc/garch/arch1.stan` | `misc/garch/arch1.data.json` | 2.0 kB | ARCH(1) as a loop over time |
| `examplemodels_cjs_cov_raneff` | `BPA/Ch.07/cjs_cov_raneff.stan` | `BPA/Ch.07/cjs_cov_raneff.data.json` | 19.2 kB | Cormack-Jolly-Seber capture-recapture with a covariate and random effects, a user-defined function, loops with branches |
| `examplemodels_cjs_mnl_ran` | `BPA/Ch.07/cjs_mnl_ran.stan` | `BPA/Ch.07/cjs_mnl_ran.data.json` | 1.2 kB | capture-recapture with multinomial-logit parameters; `multinomial_rng` in generated quantities |
| `examplemodels_covid_sens_spec` | `jupyter/covid-inf-rate/stan/hier_sens_spec_offset_mult.stan` | `jupyter/covid-inf-rate/data/santa_clara_all.data.json` | 0.3 kB | `offset` and `multiplier` on two vectors, `binomial_logit` |
| `examplemodels_dlm_fx_equicorr` | `misc/dlm/fx_equicorr.stan` | `misc/dlm/fx_equicorr.data.json` | 148.9 kB | `gaussian_dlm_obs` with `cov_matrix` parameters and a common correlation |
| `examplemodels_dlm_fx_factor` | `misc/dlm/fx_factor.stan` | `misc/dlm/fx_factor.data.json` | 148.9 kB | `gaussian_dlm_obs` with a `cov_matrix` parameter, an `inv_wishart` prior and a loading vector that is a parameter |
| `examplemodels_dlm_nile` | `misc/dlm/nile.stan` | `misc/dlm/nile.data.json` | 0.6 kB | `gaussian_dlm_obs`; local level model of 100 annual river flows |
| `examplemodels_gp_fit_multi_output` | `misc/gaussian-process/gp-fit-multi-output.stan` | `misc/gaussian-process/gp-fit-multi-output.data.json` | 7.3 kB | multi-output Gaussian process, `cholesky_factor_corr` |
| `examplemodels_hmm_marginal` | `knitr/hmm-example/hmm-example.stan` | generated | 2.3 kB | `hmm_marginal`, `hmm_hidden_state_prob` and `hmm_latent_rng`, `ordered` and `simplex` parameters |
| `examplemodels_irt_1pl_hier` | `knitr/irt/irt_1pl_hier.stan` | generated | 0.4 kB | item response theory with hierarchical difficulties |
| `examplemodels_irt_multilevel` | `knitr/irt/irt-multilevel.stan` | generated | 2.1 kB | item response theory with students in schools and item discrimination |
| `examplemodels_kidney` | `bugs_examples/vol1/kidney/kidney.stan` | `bugs_examples/vol1/kidney/kidney.data.json` | 1.6 kB | Weibull survival with right censoring and patient random effects |
| `examplemodels_mr_ss` | `BPA/Ch.08/mr_ss.stan` | `BPA/Ch.08/mr_ss.data.json` | 35.8 kB | capture-recapture in state-space form, a user-defined function, loops with branches |
| `examplemodels_ms` | `BPA/Ch.09/ms.stan` | `BPA/Ch.09/ms.data.json` | 19.3 kB | multi-state capture-recapture, a user-defined function, loops with branches |
| `examplemodels_ode_one_comp_mm` | `knitr/convert-odes/one_comp_mm_elim_abs.stan` | `knitr/convert-odes/one_comp_mm_elim_abs.data.json` | 0.5 kB | one-compartment pharmacokinetics with Michaelis-Menten elimination, `ode_bdf` (stiff solver) |
| `examplemodels_probit_multi_good` | `misc/multivariate-probit/probit-multi-good.stan` | `misc/multivariate-probit/probit-multi-good.data.json` | 88.7 kB | multivariate probit, `cholesky_factor_corr`, uniform latent parameters mapped through `Phi` and `inv_Phi` |
| `examplemodels_radon_group` | `ARM/Ch.12/radon_group.stan` | `ARM/Ch.12/radon_group.data.json` | 34.3 kB | `offset` and `multiplier` on vectors, `normal_id_glm` |
| `examplemodels_roaches_overdispersion` | `ARM/Ch.8/roaches_overdispersion.stan` | `ARM/Ch.8/roaches_overdispersion.data.json` | 5.1 kB | `multiplier` on a vector of observation-level effects, `poisson_log_glm` |
| `examplemodels_ssm` | `BPA/Ch.05/ssm.stan` | `BPA/Ch.05/ssm.data.json` | 0.5 kB | state-space population model, latent growth rates as parameters |
| `examplemodels_stagnant2` | `bugs_examples/vol2/stagnant/stagnant2.stan` | `bugs_examples/vol2/stagnant/stagnant2.data.json` | 0.4 kB | regression with a change point as a continuous parameter; the branch `x[n] < x_change` depends on a parameter |
| `examplemodels_stochvol_optimized` | `misc/moving-avg/stochastic-volatility-optimized.stan` | `misc/moving-avg/stochastic-volatility-optimized.data.json` | 9.7 kB | stochastic volatility with a non-centered latent log volatility |
| `examplemodels_sum_to_zero_binomial` | `jupyter/sum-to-zero/stan/binomial_4preds_ozs.stan` | generated | 3.5 kB | three `sum_to_zero_vector` parameters, binomial counts with test sensitivity and specificity |
| `examplemodels_sur` | `misc/sur/sur.stan` | `misc/sur/sur.data.json` | 19.7 kB | seemingly unrelated regressions, `cholesky_factor_corr` with `multi_normal_cholesky` |

For the models whose data upstream ships as `.data.json`, the JSON file here
is that file byte for byte. The data that upstream does not ship as JSON is
written by
[`tools/gen_examplemodels_data.R`](../../tools/gen_examplemodels_data.R):

- `hmm_marginal`: the two R chunks of `knitr/hmm-example/hmm-example.Rmd` that
  simulate a 100-step three-state chain, run with `set.seed(1)`.
- `sum_to_zero_binomial`: `jupyter/sum-to-zero/stan/gen_binomial_4_preds.stan`
  is a Stan program that simulates this model's data. The script is a port to R
  with the notebook's arguments. R's random number generators replace Stan's,
  so the values differ from the notebook's, and the seed is 56789.
- `irt_1pl_hier` and `irt_multilevel`: `knitr/irt/irt_1pl-sim.R` simulates the
  first. No upstream script simulates the second, so the script draws it from
  the model's own generative story. Both use `set.seed(1)`.

Rerunning `Rscript tools/gen_examplemodels_data.R` reproduces those four files
byte for byte.

## SHA-256 of the programs

| Entry | SHA-256 |
| --- | --- |
| `examplemodels_arch1` | `64c34747a5924a1dc8cd3d9587ce58564bb5c186043c431fd1565f466faec80c` |
| `examplemodels_cjs_cov_raneff` | `e8d3fdf865ebdc4c75a35f432f2ab0fb793796af870b818accd23f425c2ea175` |
| `examplemodels_cjs_mnl_ran` | `57262f6466453a56f0ba7f6d1d2a4541772a4e5ced3cd41881051182405526c5` |
| `examplemodels_covid_sens_spec` | `81459c68a45c6012402d2ef8635a6ebdd49641030362db8c50e1eb91229f48bc` |
| `examplemodels_dlm_fx_equicorr` | `be54d61d52cef94f3a28734f4f3112a805bb42bc934d4ad93205aa78bad5598e` |
| `examplemodels_dlm_fx_factor` | `7091ed7a53662cc3174332da899756e54e8b94b33bea1b0415bd3d35415d0655` |
| `examplemodels_dlm_nile` | `b1a39933bc9e9229ca0d434455243d840e9fab63874c61ef4ddd33c73b3ae83f` |
| `examplemodels_gp_fit_multi_output` | `140b865489df4f6058d5d0a04c16ea4502049c6509c61a2c79c62714b8085b31` |
| `examplemodels_hmm_marginal` | `dfebf600dc8cb783abbfd8381121402ccc213314aed60d67b1b167b21ec0b33b` |
| `examplemodels_irt_1pl_hier` | `8777be04e7e9cc4667cd1b1d95139a4d75bca82b68806d5c33e51eda6d69f93f` |
| `examplemodels_irt_multilevel` | `f0feb8460f81170e69277f6ad9608be9cc87d8f3c3050c4be1cf1fee6f2c9210` |
| `examplemodels_kidney` | `f9a38123929fd6da3517f6305813a3a5dd42984e81dbd61aa88799327cb6e98a` |
| `examplemodels_mr_ss` | `859cfb84a208bf8835a36aa17471a9dec1ab16a840159a7d8eb74485f4d8e075` |
| `examplemodels_ms` | `0970a780df0b6fe1888368b39b120180da3756a6d624a5b5f7a5e5f728f9368f` |
| `examplemodels_ode_one_comp_mm` | `b820d37230de26f03e4da509c1424253a369668261530dd6c2aef2cf4e0c04c9` |
| `examplemodels_probit_multi_good` | `378d53e701807e3479632d0e7e7ced077fe2bdfceb602e86b5baefa93a0c55be` |
| `examplemodels_radon_group` | `4d4a9e50d44e5425ce54f2f74242ca6f267336d31e6b8a8ab252f17bb16b2569` |
| `examplemodels_roaches_overdispersion` | `0e5671b817bda564e30c29a4f2524fc56ec8f87ef527c23f88a1cde958f67fce` |
| `examplemodels_ssm` | `5d125129cd1b904a3384041980626e7e6638e5f17275481182e8b32b23121fe9` |
| `examplemodels_stagnant2` | `5600076b4b3a9bd1ca78a03defd3e18f7f5f8ea98b828da98d1ed9aafe3058b6` |
| `examplemodels_stochvol_optimized` | `3357e1655dfbadf203ce5a3f19e4f0a045993a364d7da6426513e586c2e261c4` |
| `examplemodels_sum_to_zero_binomial` | `8e633707d3133d3018a5c8bea29a8d045db59a52b897d71a2fa079587f7d9e9a` |
| `examplemodels_sur` | `99d500d27414d3056d64ba0f2c87afb3f5802546aefb10db27596c7b231c604d` |

## How the references were made

The references are CmdStan 2.40.0 values at the three shared points, recorded
with [`tools/verify_sample.py`](../../tools/verify_sample.py)'s functions on
Darwin arm64 (clang), and are replayed by the corpus check. All 23 models
match at all three points within the 1e-9 scaled-error gate. The four largest
differences in ULP are `ms` (50,581 ULP, 1.4e-12 relative), `ode_one_comp_mm`
(41,984 ULP, 1.0e-13), `probit_multi_good` (4,096) and `mr_ss` (1,457). The
others are 76 ULP or less.

Two models are in `KNOWN_GAPS` in `tools/verify_refs.py` because their
generated quantities use a function that `write_array` lacks: the generated
quantities fall back to the MIR interpreter, which throws. The entries excuse
only the `write_array`, so the replay still compares the log density and
gradient of these two models at every point:

- `cjs_mnl_ran`: `unsupported function multinomial_rng`
- `hmm_marginal`: `unsupported function hmm_latent_rng`

Neither has a recorded `write_array` row, because the recorder keeps a row only
where stanli reproduces it. At `ode_one_comp_mm`'s third point the
`write_array` rows differ by 1.8e-9 relative (the ODE output of the two
engines), above the replay gate, so that point has no recorded row either.

The benchmark points in `docs/internal/artifacts/benchmark-points.json.gz` came
from a NUTS run of each model with the build that the other entries came from.
Three of these models keep the fixed points instead, with the reason recorded:
the two with `write_array` gaps above, because the run reads every column of
the generated quantities, and `probit_multi_good`, because the sampler's
initialization fails (`Phi: x is nan, but must be not nan!` in all 100
attempts), although the model's log density and gradient match CmdStan at the
three fixed points.

## Not imported

- `BPA/Ch.09/lifedead.stan` (a live-recapture and dead-recovery model) was
  recorded and then dropped: its `write_array` row has 15 MB of JSON, which
  added 1.6 MB to the compressed reference file for a model that `ms` already
  represents.
- `jupyter/sum-to-zero/stan/bym2_ozs.stan` needs spatial adjacency and a
  scaling factor that the notebook computes with `geopandas` and `libpysal`
  from a 7 MB GeoJSON file. Those packages are not available here.
- `misc/irt/*` have data files of 350 kB, above the size limit for this
  corpus.
- `regression_tests/mother.stan` has an algebraic solver and `map_rect`, but it
  is a compiler test with no statistical content.
