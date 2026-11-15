# Models from mlumr

[mlumr](https://cran.r-project.org/package=mlumr) fits multilevel unanchored
meta-regression: it compares a treatment with individual patient data against
a comparator known only from published aggregate data, integrating the
comparator's likelihood over its covariate distribution with quasi-Monte Carlo
integration points. Its survival models share a 968-line function library
(`include/survival_functions.stan`) with nine parametric distributions picked
by an integer `dist`, censoring and delayed-entry handling by an integer
status, and an M-spline baseline hazard. The integrated likelihood is a
`log_sum_exp` over integration points for each comparator row.

The models come from the CRAN release 0.2.0
(`https://cran.r-project.org/src/contrib/mlumr_0.2.0.tar.gz`). The SHA-256 of
each source file under `inst/stan/`:

    dc54571d56e1af52fc440afe55deb3b75a9873a763fcad0819a26db4c7c15793  mlumr_survival_relaxed.stan
    d655dee850a5b7e1c2a1fb35492816d7513aeebff6697be903ca1b67e97b2d1c  mlumr_survival_mspline_relaxed.stan
    c3a1a69d60bd61d0dc7399ec823bca0656d44d88bd8f91c25778cd82871d3196  include/priors_functions.stan
    e7086744a4fadff7de43baae0720df1976c0a7dd667741e93fabccc9372ea5ce  include/survival_functions.stan
    bb4a1eedd5e3b5238df2e36f1e755eff54169e5e1b6c6694cf0781fc01adcac8  include/survival_mspline_functions.stan
    eccb78dca1fcdfe4c49e4cc00195899cf37a5c498fbb0a3ac228d5361ebf0a13  include/priors_hyperparameters.stan
    2c05efc7ab074444ed3ceacdda7404900d17370023da73ff2fc6afff6a70771a  include/survival_aux_hyperparameters.stan

mlumr is by Ahmad Sofi-Mahmudi and Conor Chandler, license GPL-3, text in
[`licenses/GPL-3`](licenses/GPL-3). The NDMM example data is bundled with
mlumr and comes from multinma (GPL-3).

Each checked-in model is the package's model file with each `#include` line
replaced by the text of the file it names. Nothing else is changed.

| Entry | Model | Reaches | Parameters |
| --- | --- | --- | --- |
| `mlumr_survival_relaxed` | `mlumr_survival_relaxed.stan` | Weibull baseline (`dist = 2`) through the 9-way distribution dispatch | 12 |
| `mlumr_survival_mspline_relaxed` | `mlumr_survival_mspline_relaxed.stan` | cubic M-spline baseline with 11 spline coefficients per study, softmax simplex and random-walk smoothing prior | 32 |

The data is the survival vignette's comparison of lenalidomide
(McCarthy2012) with thalidomide (Morgan2012), with 4 covariates. The vignette
uses all 231 individual rows, 408 reconstructed comparator rows and 64
integration points, and a fit takes about 20 minutes per chain. Here every
fifth row of each arm is kept (47 and 82 rows) and there are 16 integration
points, so each gradient evaluates 47 + 82 x 16 = 1,359 survival log
likelihoods. The relaxed variant is used because the shared-coefficient
(`_spfa`) variant differs only in the coefficient vector and reaches the same
functions.

Both entries use the same data. Inlining the dispatch function at every call
site makes `mlumr_survival_relaxed` large: stanc3's `--O1` pass takes about
26 s on it, and stanli's compiler skips `--O1` for it by its structural budget
and compiles the transformed MIR instead.

The rows go through the package's own `set_ipd()`, `set_agd_surv()`,
`combine_data()`, `add_integration()` (with a fixed seed) and `mlumr()`.
`mlumr()` hands the finished list to the internal `fit_rstan()`; the script
replaces that function with one that records the model name and data and
stops, and checks that the model name is the entry's.
[`tools/gen_mlumr_data.R`](../../tools/gen_mlumr_data.R) writes the files
from the installed package after checking the hashes above. Rerunning

    Rscript tools/gen_mlumr_data.R tests/mlumr

reproduces the checked-in files byte for byte.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.
