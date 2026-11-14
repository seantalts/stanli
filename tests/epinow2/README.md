# Models from EpiNow2

[EpiNow2](https://cran.r-project.org/package=EpiNow2) estimates the
time-varying reproduction number and latent infections from case counts,
with delay distributions between infection and report. The corpus has its two
main models. They are large (1,700 and 2,600 lines once flattened) and use
`profile(...)` blocks
(20 in `estimate_infections`, 2 in `estimate_secondary`), truncated priors
`T[lb, ub]` inside a `_lp` user function with an infinite upper bound in the
data, a lower-only truncation `T[0,]`, many `void` `_lp` functions, a
Hilbert-space Gaussian process basis, and discretized delay distributions
convolved with the latent series.

The models come from the CRAN release 1.9.0
(`https://cran.r-project.org/src/contrib/EpiNow2_1.9.0.tar.gz`). The SHA-256
of every source file under `inst/stan/` that the two models use:

    67b05049cc98b1d09e2862fe603a832a174cfa248abe3abeaf0048d5872b9276  include/license.stan
    cbeff3c3e08bc3a718796d103ab41197f4764444709810cd54b602523e21a2cb  estimate_infections.stan
    9663c6a5877f050d8ca8480502f6aaad1387d83e0efc57ce801f2bb3732698e6  estimate_secondary.stan
    b1a2da27ffda8e273f33b9f75f958a769ded171d0068adcbea12bcdeafd70b22  data/backcalc.stan
    c411f5549c1c7525b5d409958a2c7fe93ba8ce6a4c0b4ca868ee128c524887c0  data/delays.stan
    262724f59f148df0f828da6811d8e18461489317ac6c7da0823bc76490be537a  data/estimate_infections_params.stan
    eedd2a6702fe91a1d4355eeb49ca492f1f40647fe5f79f31fe85063b60f91ff4  data/estimate_secondary_params.stan
    a706b67173f678b239bfda224d8f332cf58143edcf79b512ce2677c846c6f4cf  data/gaussian_process.stan
    0a4911f4ef80bd75f1e5a4e092092bffe5ca7d2a0a9f7ae262e9b378359b98d4  data/observation_model.stan
    63b78ca2bc127725c400edac0d1df66a8ccc7e9c975a46ed0841296ea1fa9ce4  data/observations.stan
    5e66f1fefc0058292cf87e17a37f396e898b2059c256f8bcf95094acc6928fed  data/params.stan
    13a46c23310efb7237cd52ac8678a683ccd395a46442afdcd0e20b6ef589a890  data/rt.stan
    e8b71ccb42abc615bbc464fa1b1e69367f9209c99ee54d608fa6a4fb27bb56df  data/secondary.stan
    476edeb59934963c68690651a3df86f317140c34b3b01a1ca15160cb10566a33  functions/convolve.stan
    81e52bc3516796e5711b59b758e942097425edf4f782c4e03d3308f559a76bce  functions/delays.stan
    69c9849dca36c469a0a3a7e3d4d970b33c3bc8815f44580783a7d1f64dabf865  functions/gaussian_process.stan
    2744520146ca7acba237cf3aa378de3fc7e177b3f8dc4ae5a34f5074f679f1a2  functions/generated_quantities.stan
    caca76a00bed8d47929d1463dfa03626450c3b40289298abc62dc1064b072d08  functions/infections.stan
    c2e3a3a46a4bad59dcc28bb57b9db73a52dda05e0ad3ab27ef36a75158ad6592  functions/observation_model.stan
    b85583c32493e3f2b2aece0400d645c6c4f84eab478c00032040dddc4f93b93b  functions/params.stan
    002517f321d869ce6b830fb50a81dfea3a0a7142611bab7fb77e5f0b97f3608c  functions/pmfs.stan
    2ac1544aa500934f382f5036a83f2b577d44e9afb8c64aeb5406b3f1f81989c8  functions/primarycensored.stan
    49746ef9301bff9806b3bd3a423c5b50ebcb7e5b930c866a73d18378faa24dcf  functions/rt.stan
    0069e4637f6261fbfbdd619a7f4f26d6e18d220d361272d42814684c57784c5f  functions/secondary.stan

Copyright (c) 2020 EpiForecasts, license MIT, text in
[`licenses/MIT`](licenses/MIT). The package's `inst/stan/include/license.stan`,
which rstantools puts at the top of the generated C++ and which is the first
block of comments in each checked-in model, is the rstantools GPL-3
boilerplate; the package's DESCRIPTION declares MIT. The file is copied
verbatim and the discrepancy is the package's.

Each model file includes 12 (`estimate_secondary`) or 18 (`estimate_infections`) files from `inst/stan/functions` and
`inst/stan/data`. The checked-in model is `include/license.stan` followed by
the model file with each `#include` line replaced by the text of the file it
names. Nothing else is changed. None of the included files include others.

| Entry | Model | Data | Parameters | Reaches |
| --- | --- | --- | --- | --- |
| `epinow2_estimate_infections` | `estimate_infections.stan` | first 35 days of `example_confirmed`, 7-day forecast horizon, 56 time points in all | 24 | Gaussian process (`fixed = 0`, 7 basis functions), Rt renewal model (`estimate_r = 1`), uncertain gamma generation time and lognormal incubation period (4 delay parameters), fixed reporting delay and truncation as discretized PMFs (`delay_id_reporting = 2`, `delay_id_truncation = 3`), one Rt breakpoint (`bp_n = 1`, which runs `bp_sd[1] ~ normal(0, 0.1) T[0,]`), 3 estimated parameters with truncated priors (upper bounds `Infinity`, 60, `Infinity`), day-of-week effect |
| `epinow2_estimate_secondary` | `estimate_secondary.stan` | first 40 days of the package's incidence example, 26 observations after the 14-day burn-in | 4 | convolution of primary to secondary counts through an uncertain lognormal delay (2 delay parameters), truncated priors on the scaling and overdispersion |

The flags in the table were checked in the generated JSON, and
`tools/gen_epinow2_data.R` stops if any of them changes.

The data is the list each function hands to Stan. `estimate_infections` uses
the package's own example settings (generation time, incubation period plus
reporting delay, Rt prior), with two additions: a truncation distribution from
the `trunc_opts()` example and one breakpoint at day 20. `estimate_secondary`
uses its documented incidence example, cut from 60 to 40 days. Both functions
build the list and then call the internal `fit_model()`; the script replaces
that function with one that records the list and stops, so nothing is
sampled. [`tools/gen_epinow2_data.R`](../../tools/gen_epinow2_data.R) writes
all four files from the installed package after checking the hashes above.
Rerunning

    Rscript tools/gen_epinow2_data.R tests/epinow2

reproduces them byte for byte.

stanli runs the generated quantities of `estimate_secondary` through its MIR
interpreter, because the compiled path does not handle a `return` there yet
(`runtime-control region: return without a function scope`). The replay
checks those values like any others.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.
