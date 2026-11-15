# Model from BeeGUTS

[BeeGUTS](https://cran.r-project.org/package=BeeGUTS) fits the General
Unified Threshold model of Survival (GUTS) to bee toxicity tests. The corpus
has its stochastic-death model, `GUTS_SD.stan`. For each treatment group it
solves a two-state ODE (scaled damage and cumulative hazard) with
`ode_rk45_tol`, with tolerances and step limit from the data. The right-hand
side finds the exposure interval by bisection with a `while` loop and
interpolates the concentration linearly. The survival counts then follow a
binomial on the conditional survival probability.

`GUTS_SD.stan` was picked over `GUTS_IT.stan` because both use the same Stan
ODE solver, and SD integrates two states with the hazard inside the ODE,
where IT integrates one state and applies a threshold distribution
afterwards. The package's `_simple` variants replace the solver with
hand-written Euler steps.

The model comes from the CRAN release 1.5.0
(`https://cran.r-project.org/src/contrib/BeeGUTS_1.5.0.tar.gz`):

    38db5557bdfa456536e5071fd6ea11e97297b6e7dcf568b88f3ac12542a28d58  GUTS_SD.stan
    97ff193cbfb114168e483dd22a13a083d72c44b41e320d6470fb71fc6ed3b3ae  include/common_stan_functions.stan
    038e74ffb208e7bc2e9555d76f68a5431b9eaf9d4f88025f310f968356756e1c  include/data_guts.stan
    4bc065156f5721c890acc4f0048f2fda04ee7946d8ce7f143b5d1e234e90d192  include/gen_quantities_guts.stan
    df8f5493dc632e0ab0d330277abb171caa5e19c6eb8d11a476243018adfd1809  include/license.stan

BeeGUTS is by Benoit Goussen and contributors, copyright Bayer AG and ibacon GmbH, license GPL-3, text in
[`licenses/GPL-3`](licenses/GPL-3). The Stan code says it is adapted from
Virgile Baudrot's gutsRstan.

The checked-in model is `GUTS_SD.stan` with each of its four `#include` lines
replaced by the text of the file it names. Nothing else is changed.

| Entry | Data | Parameters | Reaches |
| --- | --- | --- | --- |
| `beeguts_guts_sd` | `betacyfluthrinChronic`: 1 dataset, 6 treatment groups, 66 survival counts, 12 concentration points | 4 | 6 `ode_rk45_tol` solves per evaluation (one per group), bisection and interpolation inside the ODE right-hand side |

The data is the list `fitBeeGUTS(betacyfluthrinChronic, modelType = "SD")`,
the package's own example, passes to `rstan::sampling()`. The script replaces
`rstan::sampling` with a function that records the model name and data and
stops, and checks that the model is `GUTS_SD`.
[`tools/gen_beeguts_data.R`](../../tools/gen_beeguts_data.R) writes both files
from the installed package after checking the hashes above. Rerunning

    Rscript tools/gen_beeguts_data.R tests/beeguts

reproduces them byte for byte.

The model goes through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and the entry carries that as its own provenance block.
