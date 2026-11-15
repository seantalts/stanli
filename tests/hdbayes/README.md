# Models from hdbayes

[hdbayes](https://cran.r-project.org/package=hdbayes) fits generalized linear
models and survival models that borrow information from historical data sets,
through power priors, commensurate priors, latent exchangeability priors and
others. It ships 47 Stan models, each a single file with its functions written
out in full. Two are in the corpus, chosen for functions that the rest of the
corpus uses only in synthetic models:

- `lm_npp.stan`, a linear model under the normalized power prior. Its
  transformed data calls `inverse_spd` on the `cov_matrix` data field
  `cov_beta`, and every log density evaluation calls `mdivide_left_spd` and
  `multi_normal_prec_lpdf` on a precision matrix built from the parameters.
  It is the only hdbayes model that uses these three functions.
- `pwe_npp.stan`, a piecewise exponential survival model under the
  normalized power prior. Its prior on `logit(a0)` calls `lbeta`, and its
  log normalizing constant interpolates on a grid of `a0` values, finding the
  grid interval with a `while` loop whose bounds depend on the parameter `a0`.

The models come from the CRAN release 0.2.0:

- URL `https://cran.r-project.org/src/contrib/hdbayes_0.2.0.tar.gz`, SHA-256
  `4fcf717025d37c7033a351ee4a47c3eaefe4b1803d4c51be85baf38620c4c094`
- `src/stan/lm_npp.stan`, SHA-256
  `6ff96d0d018a21ac1a11258b5feac0698949f13ad698458f6eac9522cded49d5`
- `src/stan/pwe_npp.stan`, SHA-256
  `3f5b39b482a83eab7076fb4cc3b7f0b278a2b987fa3c895998d6c8f370d0c5a3`
- Copyright (c) 2024 hdbayes authors (Ethan M. Alt, Xinxin Chen, Luiz M.
  Carvalho, Joseph G. Ibrahim, Xiuya Chang), MIT license, text in
  [`licenses/MIT`](licenses/MIT)

Neither file has an `#include`, so the checked-in `.stan` files are verbatim
copies.

| Entry | Model | Data | Size | Feature |
| --- | --- | --- | --- | --- |
| `hdbayes_lm_npp` | `lm_npp.stan` | `actg019[1:100, ]` current, `actg036[1:50, ]` historical, `cd4 ~ treatment + age + race` | n 100, n0 50, p 4, K 1 | `cov_matrix` data, `inverse_spd`, `mdivide_left_spd`, `multi_normal_prec_lpdf` |
| `hdbayes_pwe_npp` | `pwe_npp.stan` | `E1690[1:50, ]` current, `E1684[1:100, ]` historical, 3 intervals | n1 50, n0 100, p 4, J 3, grid 11 | `lbeta`, parameter-dependent `while` |

The data subsets and formulas are those of the package's own examples (the
`glm.npp()` example for the ACTG data, the `pwe.npp()` example for the E1684
and E1690 data). With the default `a0_shape1 = a0_shape2 = 1`, `lm_npp`
skips its `beta` prior on `a0s`; the linear algebra above runs regardless.

`pwe.npp()` takes the log normalizing constant of the power prior as user
input, as a grid `a0.lognc` and its values `lognc`. The package's example
estimates the values with `pwe.npp.lognc()`, which runs MCMC and bridge
sampling at each grid point. The generator holds one such estimate for the
example's grid, `seq(0, 1, length.out = 11)`, made once with CmdStan 2.40.0
and seed 123 (details in the script), so regenerating needs no CmdStan.

In `pwe_npp`, `findClosestIndex` returns before the `while` loop when `a0` is
below the second grid point (0.1) or equal to the last (1). With these
estimates a 1000-draw posterior run puts `a0` between 0.15 and 0.9995, so the
loop runs at posterior draws. At random unconstrained points in (-2, 2), `a0`
is in (0.12, 0.88), and the loop runs there too.

Each `.json` file is the list that `lm.npp()` or `pwe.npp()` passes to
`$sample()`, restricted to the data block's fields.
[`tools/gen_hdbayes_data.R`](../../tools/gen_hdbayes_data.R) replaces
`instantiate::stan_package_model()` with a stub that hands that list back, so
nothing is compiled or sampled. hdbayes compiles its models with CmdStan at
install time and skips that step when no CmdStan is configured, which is
enough for the script. The installed package keeps its Stan files in
`bin/stan/`, and the script checks their hashes before writing. Rerunning

    Rscript tools/gen_hdbayes_data.R tests/hdbayes

reproduces the four files byte for byte.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.
