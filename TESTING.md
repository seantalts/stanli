# Why should I trust this?

stanli reimplements the Stan runtime, so its reference is CmdStan at the
versions pinned in [`deps/pins.env`](deps/pins.env). This page has four parts:

- [The rule](#the-rule) that every model is held to, and where CI enforces it.
- [Every model](#every-model), with the limit on each of its outputs.
- [Every check](#the-checks): what it compares, against what, when it runs and
  what it cannot see.
- [Known limits](#known-limits).

<a id="numerical-policy"></a>

## The rule

For each model, at each of three fixed points in unconstrained space, stanli
computes the log density, every gradient entry and every value that a draw
writes (constrained parameters, transformed parameters and generated
quantities, which the code calls `write_array`).

**Each of those numbers must be within 10 ULP of the number that CmdStan
computes. A number that is farther away fails the run, unless
[`tests/ulp_ledger.json`](tests/ulp_ledger.json) lists that value with the
distance that was measured and the cause.**

The same rule applies to every model in every corpus. The details:

- A ULP is one step between adjacent doubles. Both engines call the same Stan
  Math with the same floating-point flags, so most differences come from the
  order in which a sum is added.
- A point that CmdStan rejects, stanli must reject. A NaN or an infinity must
  match in kind.
- Agreement in every bit is reported and is not required.
- A ledger entry holds a measurement. Its bound is the largest distance
  measured over the three points, and `tools/verify_refs.py` can only lower
  it. Adding an entry or raising one takes a commit that gives the cause.
- Each ledger entry also says which engine is closer to an 80-digit evaluation
  of the model ([`tools/hp`](tools/hp/README.md)): `stanli`, `cmdstan` or
  `unknown`. An entry with `stanli` is settled, because stanli is at least as
  accurate as the reference it differs from. The other entries are open
  findings, and [Known limits](#known-limits) counts them.
- Within the 10, a change may move a result away from CmdStan by 2 ULP
  without comment. A change that reorders a sum may use all 10, and its commit
  message says so. No tool checks this part; reviewers do.

<a id="the-ulp-gate"></a>

### How the distance is counted

The distance between a stanli value `b` and its CmdStan value `a` is

    min(doubles between a and b, ceil(|a - b| / ulp(S)))

`S` is the largest CmdStan magnitude among the values that were computed
together with `a`: the whole gradient for a gradient entry, the other elements
of the same variable for a `write_array` value, and `|a|` itself for the log
density. The second term is there for a gradient entry that cancels to nearly
zero. One rounding of one of its terms can move such an entry by millions of
its own ULP, so it is measured against the size of the terms that formed it.
`S` comes from CmdStan's values only, so a wrong stanli value cannot widen it.

### Where CI enforces it

The ledger holds measurements from macOS arm64 only, so the 10 ULP rule is
enforced there and not yet on the other platforms. Every platform enforces a
looser limit on every value: a relative error `|a - b| / max(|a|, |b|, 1)`
below 1e-9. Linux x86-64, which is the platform that pull requests run, adds a
10 ULP limit for the <!--gen:testing_brms_limited-->124<!--/gen--> brms models
against a CmdStan recording made on Linux.

| limit | applies to | macOS arm64 | Linux x86-64 | other platforms |
| --- | --- | --- | --- | --- |
| 10 ULP or the ledger bound | every value of every model | yes | no | no |
| 10 ULP for each element, without the ledger | brms models and the `fixtures` corpus | yes | yes | `fixtures` only |
| 1e-9 relative error | every value of every model | yes | yes | yes |
| rejections and non-finite values match | every point | yes | yes | yes |

Pull requests run the replay on Linux x86-64. The other platforms run it after
a merge to `main`, and Windows never runs the main corpus.
[Known limits](#known-limits) says what this leaves unchecked.

### The limits of the other checks

Each numerical comparison has one limit and one place where its exceptions
are written down. The checks that compare with a wider tolerance, such as the
stanc3 models (1e-9) and the posterior checks, give it in
[the checks](#the-checks).

| comparison | limit | exceptions are listed in |
| --- | --- | --- |
| a model's values against CmdStan | 10 ULP | [`tests/ulp_ledger.json`](tests/ulp_ledger.json) |
| the same, on every platform | 1e-9 relative error | `ILL_CONDITIONED` and `MISMATCH_OUTPUTS` in [`tools/verify_refs.py`](tools/verify_refs.py): the <!--gen:testing_floored_models-->6<!--/gen--> models marked in the table below |
| brms models against CmdStan on the same platform | 10 ULP for each element | `ULP_LIMITS` and `GRADIENT_AGAINST_LARGEST` in `tools/verify_refs.py`, marked in the table below |
| the `fixtures` corpus against CmdStan | 10 ULP for each element | `VALUE_ULP_LIMITS` (2 models, 16 ULP for one gradient entry), and off macOS arm64 `GRADIENT_AGAINST_LARGEST_OFF_PLATFORM` (3 models) and `WRITE_ARRAY_AGAINST_LARGEST_OFF_PLATFORM` (1 model), all in `tools/verify_refs.py` |
| a model in [fast mode](#fast-mode) against CmdStan | 1e-12 relative error | `FAST_HP_CHECKED` in `tools/verify_refs.py`: 1 model |
| stanli's execution paths against each other | every bit | [`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) |
| one kernel against the Stan Math call it replaces | every bit | the budget written at the assertion |
| a generated signature case against BridgeStan | 10 ULP | the per-case entries of the conformance baseline |
| stanli's NUTS against Stan's NUTS services | every bit | none |

<a id="comparison-with-cmdstan-on-complete-models"></a>

## Every model

The main corpus has <!--gen:testing_models-->414<!--/gen--> models. CmdStan's
answers are recorded at 3 points for each model, which makes <!--gen:testing_points-->1242<!--/gen-->
points and <!--gen:testing_values-->1,504,488<!--/gen--> values. CmdStan
rejects <!--gen:testing_rejected_points-->6<!--/gen--> of the points, and
stanli must reject them too.

Of those models, <!--gen:testing_models_within-->332<!--/gen--> have every
value within 10 ULP of CmdStan. The other <!--gen:testing_models_over-->82<!--/gen-->
models have <!--gen:testing_entries-->177<!--/gen--> ledger entries between
them. In <!--gen:testing_entries_stanli-->123<!--/gen--> of those entries
stanli is at least as close to the 80-digit value as CmdStan. In <!--gen:testing_entries_cmdstan-->7<!--/gen-->
CmdStan is closer, and <!--gen:testing_entries_unknown-->47<!--/gen--> are
undecided.

<!--gen:testing_collections-->
| source | models | models with a value over 10 ULP |
| --- | ---: | ---: |
| posteriordb | 119 | 41 |
| educational | 13 | 1 |
| brms | 124 | 4 |
| rethinking | 62 | 11 |
| cogmod | 22 | 12 |
| ctsem | 3 | 1 |
| examplemodels | 23 | 4 |
| stanguide | 6 | 1 |
| oncobayes2 | 3 | 1 |
| rstanarm | 4 | 0 |
| multinma | 2 | 0 |
| epinow2 | 2 | 2 |
| mlumr | 2 | 2 |
| rbioacc | 1 | 0 |
| beeguts | 1 | 0 |
| hdbayes | 2 | 0 |
| ecoensemble | 2 | 2 |
| hbayesdm | 4 | 0 |
| bmgarch | 2 | 0 |
| walker | 2 | 0 |
| bayesdfa | 2 | 0 |
| regressions | 13 | 0 |
| all | 414 | 82 |
<!--/gen-->

The sources are posteriordb, brms formulas, the code of Statistical
Rethinking, cogmod's brms families, the Aalto Bayesian data analysis course
(`educational`), ctsem's state-space template, stan-dev/example-models, the
Stan User's Guide, 13 CRAN packages, and programs that once failed to lower
(`regressions`). [`tools/corpus_inventory.py`](tools/corpus_inventory.py) is
the inventory.

How to read the table:

- **parameters** is the number of gradient entries. **outputs** is the number
  of `write_array` values at one point.
- **lp**, **gradient** and **write_array** give the limit, in ULP, that the
  model's worst value of that kind is held to on macOS arm64. `10` is the
  rule. A larger number is a ledger bound, which is also the largest distance
  measured, followed by which engine is closer to the 80-digit value. Read
  "undecided" as "nobody has shown that stanli is right here".
- **also** gives a limit that applies as well as the first three columns.
  "brms limit 10" is the limit for each element from the table above. For some
  brms models the gradient entries are counted in ULP of the largest gradient
  entry, because the entries of mean-centred predictors cancel. An
  ill-conditioned model, or a known mismatch, has its own relative-error limit
  for the values named, on every platform.

The ledger gives the cause of each bound, value by value. The table is
generated: `python3 tools/gen_docs.py` rewrites it from the references, the
ledger and `tools/verify_refs.py`, and CI fails when they disagree.

<!--gen:testing_corpus_table-->
| model | source | parameters | outputs | lp | gradient | write_array | also |
| --- | --- | ---: | ---: | --- | --- | --- | --- |
| `2pl_latent_reg_irt` | posteriordb | 531 | 549 | 10 | 10 | 10 |  |
| `accel_gp` | posteriordb | 66 | 72 | 10 | 10 | 10 |  |
| `accel_splines` | posteriordb | 82 | 160 | 10 | 10 | 10 |  |
| `arK` | posteriordb | 7 | 7 | 10 | 10 | 10 |  |
| `arma11` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `blr` | posteriordb | 6 | 6 | 10 | 10 | 10 |  |
| `bones_model` | posteriordb | 13 | 13 | 10 | 10 | 10 |  |
| `bym2_offset_only` | posteriordb | 3845 | 9610 | 10 | 10 | 10 |  |
| `covid19imperial_v2` | posteriordb | 51 | 8457 | 10 | 10 | 10 |  |
| `covid19imperial_v3` | posteriordb | 51 | 8457 | 10 | 10 | 10 |  |
| `diamonds` | posteriordb | 26 | 27 | 10 | 10 | 168 (stanli closer) |  |
| `dogs` | posteriordb | 3 | 2253 | 59 (stanli closer) | 32 (CmdStan closer); 16 (stanli closer) | 10 |  |
| `dogs_hierarchical` | posteriordb | 2 | 752 | 15 (stanli closer) | 10 | 10 |  |
| `dogs_log` | posteriordb | 2 | 2252 | 10 | 13 (undecided) | 10 |  |
| `dogs_nonhierarchical` | posteriordb | 65 | 946 | 15 (stanli closer) | 10 | 10 |  |
| `dugongs_model` | posteriordb | 4 | 6 | 10 | 10 | 10 |  |
| `earn_height` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `eight_schools_centered` | posteriordb | 10 | 10 | 10 | 10 | 10 |  |
| `eight_schools_noncentered` | posteriordb | 10 | 18 | 10 | 10 | 10 |  |
| `election88_full` | posteriordb | 90 | 11656 | 10 | 11 (undecided); 91 (stanli closer) | 10 |  |
| `garch11` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `GLM_Binomial_model` | posteriordb | 3 | 83 | 10 | 10 | 10 |  |
| `GLM_Poisson_model` | posteriordb | 4 | 84 | 10 | 10 | 10 |  |
| `GLMM1_model` | posteriordb | 237 | 2395 | 104 (CmdStan closer) | 10 | 10 |  |
| `GLMM_Poisson_model` | posteriordb | 45 | 125 | 10 | 10 | 10 |  |
| `gp_pois_regr` | posteriordb | 13 | 24 | 10 | 10 | 10 |  |
| `gp_regr` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `gpcm_latent_reg_irt` | posteriordb | 530 | 550 | 131 (stanli closer) | 55 (undecided) | 10 |  |
| `grsm_latent_reg_irt` | posteriordb | 408 | 418 | 493 (stanli closer) | 23 (undecided) | 10 |  |
| `hier_2pl` | posteriordb | 669 | 804 | 10 | 10 | 10 |  |
| `hierarchical_gp` | posteriordb | 933 | 3279 | 10 | 10 | 10 |  |
| `hmm_drive_0` | posteriordb | 6 | 429 | 10 | 10 | 10 |  |
| `hmm_drive_1` | posteriordb | 6 | 429 | 10 | 10 | 10 |  |
| `hmm_example` | posteriordb | 4 | 111 | 10 | 10 | 10 |  |
| `hmm_gaussian` | posteriordb | 14 | 9519 | 10 | 10 | 10 |  |
| `iohmm_reg` | posteriordb | 29 | 18031 | 10 | 10 | 10 |  |
| `irt_2pl` | posteriordb | 144 | 144 | 10 | 10 | 10 |  |
| `kidscore_interaction` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `kidscore_interaction_c` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `kidscore_interaction_c2` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `kidscore_interaction_z` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `kidscore_mom_work` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `kidscore_momhs` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `kidscore_momhsiq` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `kidscore_momiq` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `kilpisjarvi` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `kronecker_gp` | posteriordb | 438 | 0 | 10 | 2.6e12 (undecided) | not compared | known mismatch: grad[1], grad[2] have their own relative limit |
| `ldaK2` | posteriordb | 7 | 12 | 171 (stanli closer) | 125 (CmdStan closer); 204 (stanli closer) | 10 |  |
| `ldaK5` | posteriordb | 7714 | 7780 | 815 (stanli closer) | 10 | 38 (stanli closer) |  |
| `log10earn_height` | posteriordb | 3 | 3 | 10 | 20 (stanli closer) | 10 |  |
| `logearn_height` | posteriordb | 3 | 3 | 10 | 15 (stanli closer) | 10 |  |
| `logearn_height_male` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `logearn_interaction` | posteriordb | 5 | 5 | 10 | 15 (stanli closer) | 10 |  |
| `logearn_interaction_z` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `logearn_logheight_male` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `logistic_regression_rhs` | posteriordb | 3075 | 4719 | 10 | 10 | 10 |  |
| `logmesquite` | posteriordb | 8 | 8 | 10 | 10 | 10 |  |
| `logmesquite_logva` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `logmesquite_logvas` | posteriordb | 8 | 8 | 10 | 10 | 10 |  |
| `logmesquite_logvash` | posteriordb | 7 | 7 | 10 | 10 | 10 |  |
| `logmesquite_logvolume` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `losscurve_sislob` | posteriordb | 15 | 384 | 10 | 10 | 10 |  |
| `lotka_volterra` | posteriordb | 8 | 90 | 10 | 10 | 10 |  |
| `low_dim_gauss_mix` | posteriordb | 5 | 5 | 14 (stanli closer) | 14 (stanli closer) | 10 |  |
| `low_dim_gauss_mix_collapse` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `lsat_model` | posteriordb | 1006 | 1012 | 10 | 65 (undecided); 70 (stanli closer) | 10 |  |
| `M0_model` | posteriordb | 2 | 4 | 66 (stanli closer) | 180 (stanli closer) | 10 |  |
| `Mb_model` | posteriordb | 3 | 1596 | 73 (stanli closer) | 68 (stanli closer) | 10 |  |
| `mesquite` | posteriordb | 8 | 8 | 10 | 10 | 10 |  |
| `Mh_model` | posteriordb | 388 | 1159 | 51 (stanli closer) | 43 (stanli closer) | 10 |  |
| `Mt_model` | posteriordb | 4 | 7 | 36 (stanli closer) | 66 (stanli closer) | 10 |  |
| `Mtbh_model` | posteriordb | 154 | 1912 | 30 (stanli closer) | 10 | 14 (undecided) |  |
| `Mth_model` | posteriordb | 394 | 5044 | 27 (stanli closer) | 28 (undecided) | 14 (undecided) |  |
| `multi_occupancy` | posteriordb | 106 | 312 | 24 (stanli closer) | 10 | 10 |  |
| `nes` | posteriordb | 10 | 10 | 10 | 10 | 10 |  |
| `nes_logit_model` | posteriordb | 2 | 2 | 10 | 10 | 10 |  |
| `nn_rbm1bJ10` | posteriordb | 7951 | 7951 | 10 | 10 | 10 |  |
| `nn_rbm1bJ100` | posteriordb | 79411 | 79411 | 10 | 10 | 10 |  |
| `normal_mixture` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `normal_mixture_k` | posteriordb | 14 | 15 | 10 | 15 (CmdStan closer) | 10 |  |
| `one_comp_mm_elim_abs` | posteriordb | 4 | 44 | 10 | 10 | 245 (undecided) |  |
| `pilots` | posteriordb | 18 | 58 | 10 | 10 | 10 |  |
| `prophet` | posteriordb | 62 | 62 | 10 | 10 | 10 |  |
| `radon_county` | posteriordb | 389 | 389 | 10 | 10 | 10 |  |
| `radon_county_intercept` | posteriordb | 388 | 388 | 95 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_hierarchical_intercept_centered` | posteriordb | 391 | 391 | 95 (stanli closer) | 213 (undecided) | 10 |  |
| `radon_hierarchical_intercept_noncentered` | posteriordb | 391 | 777 | 95 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_partially_pooled_centered` | posteriordb | 389 | 389 | 95 (stanli closer) | 229 (undecided) | 10 |  |
| `radon_partially_pooled_noncentered` | posteriordb | 389 | 775 | 95 (stanli closer) | 213 (undecided) | 10 |  |
| `radon_pooled` | posteriordb | 3 | 3 | 196 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_variable_intercept_centered` | posteriordb | 390 | 390 | 95 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_variable_intercept_noncentered` | posteriordb | 390 | 776 | 95 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_variable_intercept_slope_centered` | posteriordb | 777 | 777 | 95 (stanli closer) | 213 (undecided) | 10 |  |
| `radon_variable_intercept_slope_noncentered` | posteriordb | 777 | 1549 | 95 (stanli closer) | 213 (undecided) | 10 |  |
| `radon_variable_slope_centered` | posteriordb | 390 | 390 | 103 (stanli closer) | 213 (stanli closer) | 10 |  |
| `radon_variable_slope_noncentered` | posteriordb | 390 | 776 | 95 (stanli closer) | 213 (stanli closer) | 10 |  |
| `Rate_1_model` | posteriordb | 1 | 1 | 10 | 10 | 10 |  |
| `Rate_2_model` | posteriordb | 2 | 3 | 10 | 10 | 10 |  |
| `Rate_3_model` | posteriordb | 1 | 1 | 10 | 10 | 10 |  |
| `Rate_4_model` | posteriordb | 2 | 4 | 10 | 10 | 10 |  |
| `Rate_5_model` | posteriordb | 1 | 3 | 10 | 10 | 10 |  |
| `rats_model` | posteriordb | 65 | 66 | 10 | 10 | 10 |  |
| `seeds_centered_model` | posteriordb | 26 | 47 | 10 | 10 | 10 |  |
| `seeds_model` | posteriordb | 26 | 27 | 10 | 10 | 10 |  |
| `seeds_stanified_model` | posteriordb | 26 | 26 | 10 | 10 | 10 |  |
| `sesame_one_pred_a` | posteriordb | 3 | 3 | 10 | 36 (stanli closer) | 10 |  |
| `soil_incubation` | posteriordb | 6 | 31 | 10 | 10 | 10 |  |
| `state_space_stochastic_level_stochastic_seasonal` | posteriordb | 389 | 581 | 10 | 10 | 10 |  |
| `surgical_model` | posteriordb | 14 | 28 | 10 | 10 | 10 |  |
| `Survey_model` | posteriordb | 1 | 1002 | 10 | 71 (CmdStan closer) | 10 |  |
| `wells_daae_c_model` | posteriordb | 6 | 6 | 10 | 10 | 10 |  |
| `wells_dae_c_model` | posteriordb | 5 | 5 | 10 | 10 | 10 |  |
| `wells_dae_inter_model` | posteriordb | 7 | 7 | 10 | 10 | 10 |  |
| `wells_dae_model` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `wells_dist` | posteriordb | 2 | 2 | 10 | 118 (stanli closer) | 10 |  |
| `wells_dist100_model` | posteriordb | 2 | 2 | 10 | 10 | 10 |  |
| `wells_dist100ars_model` | posteriordb | 3 | 3 | 10 | 10 | 10 |  |
| `wells_interaction_c_model` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `wells_interaction_model` | posteriordb | 4 | 4 | 10 | 10 | 10 |  |
| `aalto_bern` | educational | 1 | 12 | 10 | 10 | 10 |  |
| `aalto_binom` | educational | 1 | 1 | 10 | 10 | 10 |  |
| `aalto_binom2` | educational | 2 | 3 | 10 | 10 | 10 |  |
| `aalto_binomb` | educational | 1 | 2 | 10 | 10 | 10 |  |
| `aalto_gpareto` | educational | 2 | 30 | 10 | 10 | 10 |  |
| `aalto_grp_aov` | educational | 4 | 4 | 10 | 10 | 10 |  |
| `aalto_grp_prior_mean` | educational | 6 | 6 | 10 | 10 | 10 |  |
| `aalto_grp_prior_mean_var` | educational | 10 | 10 | 10 | 10 | 10 |  |
| `aalto_lin` | educational | 3 | 20 | 10 | 10 | 10 |  |
| `aalto_lin_std` | educational | 3 | 29 | 10 | 10 | 10 |  |
| `aalto_lin_std_t` | educational | 4 | 30 | 10 | 10 | 10 |  |
| `aalto_poisson_hurdle` | educational | 2 | 1002 | 19 (stanli closer) | 44 (stanli closer) | 10 |  |
| `aalto_poisson_simple` | educational | 1 | 1001 | 10 | 10 | 10 |  |
| `i319_gauss_re` | brms | 66 | 127 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_negbin_fixed` | brms | 6 | 8 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_negbin_re` | brms | 66 | 127 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_fixed` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_re` | brms | 65 | 126 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_re2` | brms | 302 | 599 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i320_gp_expquad` | brms | 34 | 36 | 10 | 10 | 10 | ill-conditioned: the 1e-9 limit is 1.5e-6 for the gradient; brms limit 10 |
| `i320_gp_matern32` | brms | 34 | 36 | 10 | 10 | 10 | brms limit 10 |
| `i320_mi_nhanes` | brms | 27 | 30 | 10 | 10 | 10 | brms limit 10 |
| `i320_pois_trunc_both` | brms | 4 | 6 | 10 | 16 (stanli closer) | 10 | brms limit 16, gradient against its largest entry |
| `i320_pois_trunc_ub` | brms | 4 | 6 | 10 | 16 (stanli closer) | 10 | brms limit 16, gradient against its largest entry |
| `i320_sratio_cs` | brms | 8 | 13 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `i320_sratio_plain` | brms | 6 | 11 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_ar_cov` | brms | 4 | 70 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_beta_binomial` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10 |
| `s2_car` | brms | 10 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_car_esicar` | brms | 8 | 15 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_car_icar` | brms | 9 | 16 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_categorical_re` | brms | 16 | 29 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cens_interval` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_com_poisson` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10 |
| `s2_cosy` | brms | 4 | 70 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cox` | brms | 6 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cox_cens` | brms | 6 | 9 | 10 | 10 | 10 | brms limit 10 |
| `s2_cumulative_cauchit` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cumulative_cloglog` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10 |
| `s2_cumulative_probit` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10 |
| `s2_custom_vint` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10 |
| `s2_custom_vreal` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_dirichlet` | brms | 5 | 8 | 10 | 10 | 10 | brms limit 10 |
| `s2_discrete_weibull` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_dist_sigma_re` | brms | 9 | 17 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_fcor` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_frechet` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10 |
| `s2_gev` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_gp_approx` | brms | 9 | 11 | 10 | 10 | 10 | brms limit 10 |
| `s2_gp_by_approx` | brms | 16 | 18 | 10 | 10 | 10 | brms limit 10 |
| `s2_gp_by_gr` | brms | 52 | 54 | 10 | 10 | 10 | ill-conditioned: the 1e-9 limit is 1.5e-6 for the gradient; brms limit 10 |
| `s2_gr_by` | brms | 10 | 17 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_gr_student` | brms | 15 | 27 | 10 | 10 | 10 | brms limit 10 |
| `s2_hurdle_cumulative` | brms | 4 | 8 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_hurdle_negbin` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10 |
| `s2_index_mi` | brms | 9 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_invgaussian` | brms |  |  |  |  |  | CmdStan rejects every point, and stanli must too |
| `s2_logistic_normal` | brms | 7 | 18 | 10 | 10 | 10 | brms limit 10 |
| `s2_me2` | brms | 89 | 259 | 10 | 10 | 10 | brms limit 10 |
| `s2_me2_nomecor` | brms | 88 | 170 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mi_lognormal` | brms | 9 | 12 | 10 | 10 | 64 (stanli closer) | brms limit 64 |
| `s2_mi_trunc_lb` | brms | 9 | 12 | 10 | 10 | 64 (stanli closer) | brms limit 64 |
| `s2_mixture_theta` | brms | 8 | 14 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mm` | brms | 9 | 16 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mm_weights` | brms | 9 | 16 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mmc` | brms | 16 | 46 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mo_simo_prior` | brms | 5 | 8 | 10 | 10 | 10 | brms limit 10 |
| `s2_multinomial` | brms | 4 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mv_shared_re` | brms | 19 | 50 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mv_subset` | brms | 6 | 9 | 10 | 10 | 10 | brms limit 10 |
| `s2_nl_noloop` | brms | 5 | 6 | 10 | 10 | 10 | brms limit 10 |
| `s2_nlf` | brms | 3 | 4 | 10 | 10 | 10 | CmdStan rejects point 1 and 2, and stanli must too; brms limit 10 |
| `s2_rate` | brms | 2 | 4 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_s_by` | brms | 52 | 94 | 10 | 10 | 10 | brms limit 10 |
| `s2_s_cc` | brms | 11 | 21 | 10 | 10 | 10 | brms limit 10 |
| `s2_sar` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_sar_error` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_shifted_lognormal` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10 |
| `s2_t2_by` | brms | 56 | 100 | 10 | 10 | 10 | brms limit 10 |
| `s2_threading` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10 |
| `s2_unstr` | brms | 31 | 161 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_weights_trunc` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_wiener` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_zi_asymlaplace` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10 |
| `s2_zi_beta` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `s2_zoi_beta` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_acat` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_acat_cs` | brms | 7 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_ar` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_arma` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_asymlaplace` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10 |
| `sw_bernoulli` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_beta` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10 |
| `sw_binomial` | brms | 2 | 4 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_categorical` | brms | 4 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cens` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cratio` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cratio_cs` | brms | 7 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cumulative` | brms | 5 | 10 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cumulative_cs` | brms | 7 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_dist_sigma` | brms | 4 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_exgaussian` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gamma` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gaussian` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gp` | brms | 44 | 46 | 10 | 10 | 10 | ill-conditioned: the 1e-9 limit is 1.5e-6 for the gradient; brms limit 10 |
| `sw_hurdle_gamma` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_hurdle_lognormal` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_hurdle_pois` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_lognormal` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_ma` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_me` | brms | 45 | 87 | 10 | 10 | 10 | brms limit 10 |
| `sw_mi` | brms | 9 | 12 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mixture` | brms | 7 | 15 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mono` | brms | 5 | 8 | 10 | 10 | 10 | brms limit 10 |
| `sw_mv_norescor` | brms | 6 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mv_rescor` | brms | 7 | 18 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_negbinomial` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_nonlinear` | brms | 3 | 4 | 10 | 10 | 10 | brms limit 10 |
| `sw_poisson` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_bern` | brms | 8 | 15 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_gauss` | brms | 9 | 16 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_negbin` | brms | 9 | 16 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_pois` | brms | 8 | 15 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_slope` | brms | 16 | 46 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_se` | brms | 2 | 5 | 10 | 10 | 10 | brms limit 10 |
| `sw_skewnormal` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_spline_s` | brms | 12 | 22 | 10 | 10 | 10 | brms limit 10 |
| `sw_spline_t2` | brms | 29 | 52 | 10 | 10 | 10 | brms limit 10 |
| `sw_sratio` | brms | 4 | 9 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_student` | brms | 5 | 7 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_trunc` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_vonmises` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_weibull` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_weights` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_binomial` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_negbin` | brms | 4 | 6 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_poisson` | brms | 3 | 5 | 10 | 10 | 10 | brms limit 10, gradient against its largest entry |
| `ch09_m5_8s` | rethinking | 4 | 4 | 10 | 10 | 10 |  |
| `ch09_m5_8s2` | rethinking | 4 | 4 | 10 | 10 | 10 |  |
| `ch09_m9_1` | rethinking | 5 | 5 | 10 | 10 | 10 |  |
| `ch09_m9_1_chains4` | rethinking | 5 | 5 | 10 | 10 | 10 |  |
| `ch09_m9_2` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch09_m9_3` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch09_m9_4` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch09_m9_5` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch09_mp` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch11_m11_10` | rethinking | 4 | 24 | 10 | 10 | 10 |  |
| `ch11_m11_11` | rethinking | 5 | 25 | 10 | 10 | 10 |  |
| `ch11_m11_4` | rethinking | 11 | 1019 | 10 | 10 | 10 |  |
| `ch11_m11_5` | rethinking | 11 | 1019 | 10 | 14 (stanli closer) | 10 |  |
| `ch11_m11_6` | rethinking | 11 | 67 | 10 | 10 | 10 |  |
| `ch11_m11_7` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch11_m11_8` | rethinking | 8 | 8 | 10 | 10 | 10 |  |
| `ch11_m11_9` | rethinking | 1 | 12 | 10 | 10 | 10 |  |
| `ch11_m_pois` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch12_m12_1` | rethinking | 3 | 4 | 10 | 10 | 10 |  |
| `ch12_m12_2` | rethinking | 6 | 26 | 10 | 10 | 10 |  |
| `ch12_m12_3` | rethinking | 2 | 2 | 35 (stanli closer) | 78 (stanli closer) | 10 |  |
| `ch12_m12_3_alt` | rethinking | 2 | 2 | 20 (stanli closer) | 138 (stanli closer) | 10 |  |
| `ch12_m12_4` | rethinking | 6 | 6 | 518 (stanli closer) | 156 (stanli closer) | 10 |  |
| `ch12_m12_5` | rethinking | 11 | 11 | 10 | 10 | 157 (stanli closer) |  |
| `ch12_m12_6` | rethinking | 16 | 17 | 10 | 157 (stanli closer) | 10 |  |
| `ch12_m12_7` | rethinking | 10 | 10 | 10 | 10 | 157 (stanli closer) |  |
| `ch13_m13_1` | rethinking | 48 | 144 | 10 | 10 | 10 |  |
| `ch13_m13_2` | rethinking | 50 | 146 | 10 | 10 | 10 |  |
| `ch13_m13_3` | rethinking | 62 | 62 | 10 | 10 | 10 |  |
| `ch13_m13_4` | rethinking | 20 | 1028 | 10 | 10 | 10 |  |
| `ch13_m13_4b` | rethinking | 20 | 20 | 10 | 10 | 10 |  |
| `ch13_m13_4nc` | rethinking | 20 | 33 | 10 | 10 | 10 |  |
| `ch13_m13_5` | rethinking | 13 | 1021 | 10 | 10 | 10 |  |
| `ch13_m13_6` | rethinking | 21 | 1029 | 10 | 10 | 10 |  |
| `ch13_m13_7` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `ch13_m13_7nc` | rethinking | 2 | 3 | 10 | 10 | 10 |  |
| `ch14_m14_1` | rethinking | 46 | 49 | 10 | 10 | 10 |  |
| `ch14_m14_10` | rethinking | 4 | 4 | 10 | 10 | 10 |  |
| `ch14_m14_11` | rethinking | 5 | 5 | 10 | 10 | 10 |  |
| `ch14_m14_2` | rethinking | 76 | 96 | 10 | 10 | 10 |  |
| `ch14_m14_3` | rethinking | 76 | 1188 | 10 | 10 | 10 |  |
| `ch14_m14_4` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch14_m14_4x` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch14_m14_5` | rethinking | 4 | 4 | 10 | 10 | 10 |  |
| `ch14_m14_6` | rethinking | 7 | 10 | 10 | 10 | 10 |  |
| `ch14_m14_6x` | rethinking | 7 | 10 | 10 | 10 | 10 |  |
| `ch14_m14_7` | rethinking | 656 | 1266 | 10 | 13 (stanli closer) | 10 |  |
| `ch14_m14_8` | rethinking | 15 | 15 | 10 | 10 | 10 |  |
| `ch14_m14_8nc` | rethinking | 15 | 225 | 10 | 10 | 10 |  |
| `ch14_m14_9` | rethinking | 4 | 4 | 10 | 10 | 10 |  |
| `ch15_m15_1` | rethinking | 54 | 54 | 10 | 10 | 10 |  |
| `ch15_m15_2` | rethinking | 104 | 104 | 10 | 10 | 10 |  |
| `ch15_m15_3` | rethinking | 2 | 2 | 10 | 83 (stanli closer) | 10 |  |
| `ch15_m15_4` | rethinking | 2 | 2 | 10 | 81 (stanli closer) | 10 |  |
| `ch15_m15_5` | rethinking | 18 | 18 | 10 | 10 | 10 |  |
| `ch15_m15_6` | rethinking | 6 | 6 | 10 | 10 | 10 |  |
| `ch15_m15_7` | rethinking | 21 | 24 | 10 | 10 | 10 |  |
| `ch15_m15_8` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch15_m15_9` | rethinking | 3 | 303 | 10 | 11 (stanli closer) | 10 |  |
| `ch16_m16_1` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `ch16_m16_4` | rethinking | 3 | 3 | 10 | 10 | 10 |  |
| `extra_hurdle_poisson` | rethinking | 2 | 2 | 10 | 10 | 10 |  |
| `cm_betadiscrete` | cogmod | 4 | 8 | 10 | 10 | 10 |  |
| `cm_betagate` | cogmod | 5 | 10 | 10 | 10 | 10 |  |
| `cm_bisa` | cogmod | 5 | 10 | 10 | 18 (stanli closer) | 10 |  |
| `cm_choco` | cogmod | 9 | 18 | 10 | 18 (stanli closer) | 10 |  |
| `cm_ddm` | cogmod | 9 | 18 | 10 | 19 (undecided) | 10 |  |
| `cm_exgaussian` | cogmod | 4 | 8 | 10 | 10 | 10 |  |
| `cm_exwald` | cogmod | 6 | 12 | 10 | 30 (stanli closer) | 10 |  |
| `cm_gamma` | cogmod | 5 | 10 | 10 | 16 (stanli closer) | 10 |  |
| `cm_geg` | cogmod | 5 | 10 | 10 | 10 | 10 |  |
| `cm_invgamma` | cogmod | 5 | 10 | 10 | 10 | 10 |  |
| `cm_invgaussian` | cogmod | 7 | 14 | 10 | 16 (stanli closer) | 10 |  |
| `cm_invweibull` | cogmod | 5 | 10 | 10 | 10 | 10 |  |
| `cm_lba1` | cogmod | 7 | 14 | 10 | 17 (stanli closer) | 10 |  |
| `cm_lba2` | cogmod | 9 | 18 | 10 | 19 (stanli closer) | 10 |  |
| `cm_lnr` | cogmod | 8 | 16 | 10 | 32 (stanli closer) | 10 |  |
| `cm_lnr_bench` | cogmod | 9 | 16 | 10 | 10 | 10 |  |
| `cm_loggamma` | cogmod | 6 | 12 | 10 | 10 | 10 |  |
| `cm_lognormal` | cogmod | 6 | 12 | 10 | 10 | 10 |  |
| `cm_logstudent` | cogmod | 6 | 12 | 10 | 28 (stanli closer) | 10 |  |
| `cm_logweibull` | cogmod | 5 | 10 | 10 | 10 | 10 |  |
| `cm_rdm` | cogmod | 7 | 14 | 10 | 18 (stanli closer) | 10 |  |
| `cm_weibull` | cogmod | 5 | 10 | 10 | 17 (stanli closer) | 10 |  |
| `ctsem_ctsm` | ctsem | 181 | 648 | 10 | 10 | 10 |  |
| `ctsem_ctsm_400` | ctsem | 1621 | 4968 | 10 | 10 | 10 |  |
| `ctsem_ctsm_post4_100` | ctsem | 580 | 4929 | 10 | 10 | 22 (undecided) |  |
| `examplemodels_arch1` | examplemodels | 3 | 3 | 10 | 10 | 10 |  |
| `examplemodels_cjs_cov_raneff` | examplemodels | 23 | 16574 | 10 | 19 (stanli closer) | 10 |  |
| `examplemodels_cjs_mnl_ran` | examplemodels | 21 | 0 | 10 | 10 | not compared |  |
| `examplemodels_covid_sens_spec` | examplemodels | 23 | 41 | 10 | 10 | 10 |  |
| `examplemodels_dlm_fx_equicorr` | examplemodels | 7 | 25 | 10 | 10 | 10 |  |
| `examplemodels_dlm_fx_factor` | examplemodels | 7 | 10 | 10 | 10 | 10 |  |
| `examplemodels_dlm_nile` | examplemodels | 2 | 2 | 10 | 10 | 10 |  |
| `examplemodels_gp_fit_multi_output` | examplemodels | 311 | 326 | 10 | 10 | 10 | ill-conditioned: the 1e-9 limit is 1.5e-6 for the gradient and 5e-8 for lp |
| `examplemodels_hmm_marginal` | examplemodels | 10 | 0 | 10 | 10 | not compared |  |
| `examplemodels_irt_1pl_hier` | examplemodels | 33 | 33 | 10 | 10 | 10 |  |
| `examplemodels_irt_multilevel` | examplemodels | 55 | 55 | 10 | 10 | 10 |  |
| `examplemodels_kidney` | examplemodels | 46 | 51 | 10 | 10 | 10 |  |
| `examplemodels_mr_ss` | examplemodels | 2 | 19602 | 10 | 729 (stanli closer) | 10 |  |
| `examplemodels_ms` | examplemodels | 6 | 72036 | 10 | 254 (stanli closer) | 10 |  |
| `examplemodels_ode_one_comp_mm` | examplemodels | 4 | 44 | 10 | 30 (undecided) | 13 (undecided) |  |
| `examplemodels_probit_multi_good` | examplemodels | 2038 | 2064 | 10 | 10 | 10 |  |
| `examplemodels_radon_group` | examplemodels | 92 | 92 | 10 | 10 | 10 |  |
| `examplemodels_roaches_overdispersion` | examplemodels | 267 | 267 | 10 | 10 | 10 |  |
| `examplemodels_ssm` | examplemodels | 28 | 55 | 10 | 10 | 10 |  |
| `examplemodels_stagnant2` | examplemodels | 5 | 5 | 10 | 10 | 10 |  |
| `examplemodels_stochvol_optimized` | examplemodels | 503 | 1003 | 10 | 10 | 10 |  |
| `examplemodels_sum_to_zero_binomial` | examplemodels | 19 | 833 | 10 | 10 | 10 |  |
| `examplemodels_sur` | examplemodels | 27 | 33 | 10 | 10 | 10 |  |
| `stanguide_changepoint` | stanguide | 2 | 114 | 10 | 10 | 10 |  |
| `stanguide_cjs_history` | stanguide | 5 | 8 | 10 | 10 | 10 |  |
| `stanguide_copula` | stanguide | 6 | 21 | 10 | 10 | 10 |  |
| `stanguide_integrate_1d_normal` | stanguide | 3 | 3 | 10 | 10 | 10 |  |
| `stanguide_map_rect_logistic` | stanguide | 2 | 2 | 10 | 10 | 10 |  |
| `stanguide_ode_adjoint` | stanguide | 5 | 5 | 10 | 2.0e10 (undecided) | 10 | known mismatch: grad[1], grad[2], grad[5] have their own relative limit |
| `oncobayes2_blrm_exnex_combo2` | oncobayes2 | 52 | 143 | 10 | 10 | 10 |  |
| `oncobayes2_blrm_exnex_combo3` | oncobayes2 | 99 | 271 | 10 | 10 | 14 (undecided) |  |
| `oncobayes2_blrm_exnex_sa` | oncobayes2 | 9 | 23 | 10 | 10 | 10 |  |
| `rstanarm_glm_gaussian` | rstanarm | 4 | 9 | 10 | 10 | 10 |  |
| `rstanarm_glmer_bernoulli` | rstanarm | 71 | 142 | 10 | 10 | 10 |  |
| `rstanarm_glmer_nb_count` | rstanarm | 25 | 51 | 10 | 10 | 10 |  |
| `rstanarm_polr` | rstanarm | 9 | 113 | 10 | 10 | 10 | CmdStan rejects point 2, and stanli must too |
| `multinma_ndmm_mspline` | multinma | 62 | 956 | 10 | 10 | 10 |  |
| `multinma_smoking_re` | multinma | 59 | 375 | 10 | 10 | 10 |  |
| `epinow2_estimate_infections` | epinow2 | 24 | 352 | 10 | 34,157 (undecided) | 24 (undecided) |  |
| `epinow2_estimate_secondary` | epinow2 | 4 | 72 | 68 (undecided) | 18 (CmdStan closer) | 10 |  |
| `mlumr_survival_mspline_relaxed` | mlumr | 32 | 1052 | 10 | 10 | 128 (undecided) |  |
| `mlumr_survival_relaxed` | mlumr | 12 | 925 | 10 | 10 | 16 (stanli closer) |  |
| `rbioacc_odetk` | rbioacc | 3 | 124 | 10 | 10 | 10 |  |
| `beeguts_guts_sd` | beeguts | 4 | 540 | 10 | 10 | 10 |  |
| `hdbayes_lm_npp` | hdbayes | 6 | 6 | 10 | 10 | 10 |  |
| `hdbayes_pwe_npp` | hdbayes | 8 | 64 | 10 | 10 | 10 |  |
| `ecoensemble_hierarchical_withdrivers` | ecoensemble | 63 | 612 | 10 | 10 | 16 (undecided) |  |
| `ecoensemble_hierarchical_withdrivers_conj` | ecoensemble | 63 | 612 | 10 | 10 | 16 (undecided) |  |
| `hbayesdm_dd_hyperbolic` | hbayesdm | 44 | 2266 | 10 | 10 | 10 |  |
| `hbayesdm_gng_m4` | hbayesdm | 72 | 14548 | 10 | 10 | 10 |  |
| `hbayesdm_igt_pvl_delta` | hbayesdm | 24 | 448 | 10 | 10 | 10 |  |
| `hbayesdm_prl_rp` | hbayesdm | 66 | 8149 | 10 | 10 | 10 |  |
| `bmgarch_bekk_arma` | bmgarch | 27 | 0 | 10 | 10 | not compared |  |
| `bmgarch_ccc` | bmgarch | 22 | 0 | 10 | 10 | not compared |  |
| `walker_rw1_model` | walker | 4 | 410 | 10 | 10 | 10 |  |
| `walker_rw1_model_naive` | walker | 304 | 607 | 10 | 10 | 10 |  |
| `bayesdfa_dfa` | bayesdfa | 25 | 416 | 10 | 10 | 10 |  |
| `bayesdfa_dfa_gp` | bayesdfa | 13 | 0 | 10 | 10 | not compared |  |
| `empty_return_shape` | regressions | 3 | 4 | 10 | 10 | 10 |  |
| `rbmi_mmrm` | regressions | 4 | 15 | 10 | 10 | 10 |  |
| `reject_udf_data_size` | regressions | 1 | 1 | 10 | 10 | 10 |  |
| `tuple_array_params` | regressions | 54 | 109 | 10 | 10 | 10 |  |
| `tuple_coef_cont` | regressions | 40 | 49 | 10 | 10 | 10 |  |
| `tuple_decompositions` | regressions | 9 | 30 | 10 | 10 | 10 |  |
| `tuple_decompositions_more` | regressions | 3 | 68 | 10 | 10 | 10 |  |
| `tuple_effect_once` | regressions | 1 | 4 | 10 | 10 | 10 |  |
| `tuple_loops` | regressions | 6 | 96 | 10 | 10 | 10 |  |
| `tuple_reduce_sum` | regressions | 4 | 5 | 10 | 10 | 10 |  |
| `tuple_return_slices` | regressions | 4 | 100 | 10 | 10 | 10 |  |
| `while_counter_size` | regressions | 7 | 8 | 10 | 10 | 10 |  |
| `while_udf_slices` | regressions | 6 | 6 | 10 | 10 | 10 |  |
<!--/gen-->

### The local corpora

CTest replays seven smaller corpora with the same tool and the same rule
(`tools/verify_refs.py --corpus NAME`). Their references are recorded from
CmdStan in the same way.

| corpus | models | CTest |
| --- | --- | --- |
| `functions` | [10 models that call 468 function names](tests/function_coverage/README.md), including data-only helpers, RNGs, solvers and five operators | `test_function_models` |
| `signatures` | 74 generated models that cover 19,642 signatures, with one output for each case | `signature_refs_*` |
| `sized_functions` | 5 models that call library functions at real sizes | `test_sized_functions_reference` |
| `container_flow` | 66 models that write each container shape in each block and read it back | `test_container_flow_reference` |
| `fixtures` | 63 models from `tests/fixtures`, also held to 10 ULP for each element on every platform | `*_reference` |
| `cmdstan_replay` | 14 models for lowering rewrites that only stock stanc's `--O1` MIR reaches | `test_cmdstan_replay` |
| `oncobayes2` | the 3 OncoBayes2 models | `test_oncobayes2_reference` |

`container_flow`, `cmdstan_replay` and `oncobayes2` run a second time through
stock stanc (the `_stanc` CTests, and the `--stanc` rows below). A model also
fails if it runs through the MIR interpreter when its corpus does not list it
as interpreted, or the reverse. These are all the values in the local corpora
that have a ledger entry:

<!--gen:testing_local_ledger-->
| corpus | model | value | limit | reason |
| --- | --- | --- | --- | --- |
| `container_flow` | `container_flow_rt_fn_amm` | `lp` | 17 (stanli closer) | 80-digit reference at point 1 (exact arithmetic on the model's constants): stanli 1, CmdStan 16 scaled ULP from the true value; stanli is at least as close to it; the merged loop's sums are pairwise in stanli, while CmdStan adds one term at a time |
| `container_flow --stanc` | `container_flow_rt_fn_amm` | `lp` | 17 (stanli closer) | 80-digit reference at point 1 (exact arithmetic on the model's constants): stanli 1, CmdStan 16 scaled ULP from the true value; stanli is at least as close to it; the merged loop's sums are pairwise in stanli, while CmdStan adds one term at a time |
| `fixtures` | `nested_callback_contexts` | `grad[2]` | 16 (undecided) | the adjoint ODE sensitivity in the combined rate gradient, as VALUE_ULP_LIMITS records; both callback engines agree |
| `fixtures` | `nested_callback_contexts_fallback` | `grad[2]` | 16 (undecided) | the adjoint ODE sensitivity in the combined rate gradient, as VALUE_ULP_LIMITS records; both callback engines agree |
| `oncobayes2` | `oncobayes2_blrm_exnex_combo3` | `wa beta_EX_prob.2.2` | 14 (undecided) | cause not yet explained |
| `oncobayes2 --stanc` | `oncobayes2_blrm_exnex_combo3` | `wa beta_EX_prob.2.2` | 14 (undecided) | cause not yet explained |
| `signatures` | `builtin_signatures_20` | `lp` | 20 (stanli closer) | the lp is checked against the exact lp of the recorded probe and case values, and at point 0 it cancels: its parts add up to 28 times |lp|, and a plain sum of std_normal's 360 squares alone rounds by 40.0 ULP of lp; stanli is 19.5 and CmdStan's recorded lp 27.5 ULP from that exact value |
| `signatures` | `builtin_signatures_24` | `lp` | 38 (stanli closer) | the lp is checked against the exact lp of the recorded probe and case values, and at point 0 it cancels: its parts add up to 21 times |lp|, and a plain sum of std_normal's 360 squares alone rounds by 80.1 ULP of lp; stanli is 37.7 and CmdStan's recorded lp 45.7 ULP from that exact value |
<!--/gen-->

A signature model's log density is compared with the exact sum of its own
recorded terms, because CmdStan's in-order sum of 360 to 1,440 terms carries
its own rounding.

<a id="checks-run-before-and-after-merge"></a>

## The checks

"PR" means a pull request that changes source. It builds and tests on Linux
x86-64 with Clang and on Windows x64. A pull request that touches only
documentation runs only the static checks, and one that touches only the R
package runs the static checks and the R tests. "Post-merge" means pushes to
`main`, nightly runs, release tags and manual runs. Those add macOS arm64 and
x86-64, Linux aarch64 and a GCC build on Linux x86-64. To run them on a
branch, use `gh workflow run wheels.yml --ref BRANCH`. "CTest" means the check
is a CTest and runs wherever the suite runs. The jobs are in
[`.github/workflows`](.github/workflows).

**Against CmdStan**

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [corpus replay](#every-model) | log density, gradient and `write_array` rows of <!--gen:testing_models-->414<!--/gen--> models at 3 points | recorded CmdStan values, under [the rule](#the-rule) | PR on Linux x86-64; post-merge on macOS arm64 and x86-64, Linux aarch64 and Linux GCC | parameter values away from the 3 points; Windows; the 10 ULP rule on any platform but macOS arm64 |
| [fast-mode replay](#fast-mode) | the same models compiled in fast mode | the same references, fast-mode limits | PR and post-merge, Linux x86-64 only | macOS, Windows and ARM |
| [local corpus replays](#the-local-corpora) | log density, gradient and outputs of the function, signature, fixture and other local models | recorded CmdStan values | PR (CTest) | signatures and sizes the generators exclude |
| [stanc3 good models](#stan-language-coverage) | the 1,382 models in stanc3's `good` integration tests at up to 5 points | recorded CmdStan values and messages, 1e-9; a status ledger | PR (CTest) | models whose generated data CmdStan rejects |
| [stanc3 bad models](#stan-language-coverage) | compile errors of the 1,200 models in stanc3's `bad` integration tests | stanc3's expected output; a ledger | PR (CTest) | |
| [generated conformance sweep](#stan-language-coverage) | 24,322 generated signature and construct cases, and each case's status | reference BridgeStan from the same pins; the checked-in baseline | nightly | operator-only behavior; cases the generator cannot build |
| [kernel unit tests](#unit-tests) | one operation's value and every gradient component | the stan-math `var` call CmdStan's generated C++ would make, with the limit stated at each assertion | PR (CTest) | whether lowering selects that kernel |
| [lowering and pass tests](#unit-tests) | log density, gradient, `write_array` rows and op counts of small compiled models | stan-math `var` references, hand-derived constants or the MIR interpreter | PR (CTest) | models outside the fixtures |
| [lit](#unit-tests) | each marked `.stan` file's compile or evaluation result, and optionally one pass dump | the expectation written in the file | PR (CTest) | values the file does not state |
| [fused-density differential](#unit-tests) | the seven fused densities over activity masks, shapes and edge values | Stan Math's kernels; every bit, 2 ULP for normal and cauchy | Linux x86-64 on PRs and post-merge; AddressSanitizer; Windows ARM64 dev setup | macOS |
| BridgeStan C ABI | 4 fixture models: values, names, counts, shapes | reference BridgeStan | post-merge | other models |
| WebAssembly replay | log density and gradient of the corpus through the wasm build | the same references and limits | by hand | `write_array`; `nn_rbm1bJ100`, whose compile does not fit in 4 GB |

**stanli against itself**

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [cross-path matrix](#comparing-stanlis-execution-paths) | every fixture at 3 points and the 22 cogmod models at 13, under 19 execution configurations | each other, in every bit, with a ledger | PR on Linux x86-64, in the build with developer switches; post-merge under AddressSanitizer and on Windows ARM64 | transformed data, which has one engine; an error every path shares |
| [posteriordb cross-path](#comparing-stanlis-execution-paths) | the posteriordb models under the same configurations plus one pass-off set | each other, within 1e-11 | by hand after pass changes | an error present in both cells |
| [compiler producer parity](#stan-language-coverage) | compact MIR bytes from the native, JavaScript and Windows compilers | each other, byte for byte | PR | numerical behavior |
| stanc3 fuzzer ([`tools/fuzz_stanc`](tools/fuzz_stanc/README.md)) | random Stan programs compiled with and without stanc3's O1 passes: log density, gradient, `write_array`, printed output and errors at 3 points | each other | nightly, Linux x86-64 | programs that the generator cannot write |
| Rethinking regeneration | the 62 Rethinking fixtures, regenerated from the book's code with pinned `rethinking` | the checked-in files, byte for byte | nightly; pushes to `main` that touch them | |
| MIR wire cost | compact MIR decode time and size on Eight Schools | legacy MIR; each must be at most half | post-merge | other models |

**Samplers and algorithms**

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [sampler parity](#samplers-and-other-algorithms) | NUTS draws and statistics across metrics and options | Stan's own NUTS services on the same executor, in every bit | PR (CTest) | compiler numerics (both sides share the executor) |
| [algorithm checks](#samplers-and-other-algorithms) | optimize, Laplace, Pathfinder, ADVI, fixed_param, RNG streams | a known Gaussian posterior; Stan's generator | PR (CTest) | non-Gaussian posteriors |
| [reference posteriors](#samplers-and-other-algorithms) | NUTS means and standard deviations of the 39 posteriordb models with reference draws | posteriordb reference draws, within 4 Monte Carlo standard errors | by hand | WALNUTS; models without reference draws |
| [sampler trace](#samplers-and-other-algorithms) | step counts, tree depth, step size, divergences, mean `lp__` | CmdStan with the same settings | by hand after sampler changes | statistical validity of NUTS |
| [sampling smoke](#every-model) | 100 saved draws of the 13 Aalto fixtures | reference output names, draw count, finiteness | PR (CTest) | convergence |
| [brms end to end](#samplers-and-other-algorithms) | `brm()` fits of the 124 brms formulas with the `stanli` backend | the same fits with brms's `cmdstanr` backend | by hand | anything beyond posterior means; platforms other than macOS arm64 |

**Packages, platforms and memory**

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [installed clients](#installed-clients) | the built wheel, R package and BridgeStan transport | API tests and R ecosystem consumers; skips fail | PR: wheel on Linux x86-64 and Windows x64, R on Linux. Post-merge: wheel on macOS and Linux aarch64, R ecosystem subset on macOS arm64 and Windows x64 | the R suite with a runtime on macOS and Windows |
| browser runtime | `tests/test_wasm.cjs` and `tests/test_worker_diagnostics.cjs`: compile, evaluate and sample Eight Schools through the browser build and its worker | expected values and API behavior | pushes to `main`; `test_wasm.cjs` again on release tags | pull requests and nightly runs |
| webR load | the WebAssembly side module | webR's loader and the C API symbols | post-merge | the R package flow |
| downstream `stanr` | `stanr` built against this runtime, its backend tests | its own expectations | on demand only | other R consumers |
| no-stdio build | undefined symbols of the runtime objects (via [`tools/check_no_stdio.sh`](tools/check_no_stdio.sh)) | the stdout, `printf`, `abort` and `exit` symbols `R CMD check` rejects | post-merge | |
| Windows dev setup | [`tools/dev_setup.sh`](tools/dev_setup.sh) on Windows, then CTest | the full suite on ARM64 (through `stanli-compile`); six tests on x64 with the embedded compiler | pushes to `main`, on demand | the rest of the suite with the embedded compiler on x64 |
| AddressSanitizer | the CTest suite, compiling through `stanli-compile` | no diagnostics | post-merge except release tags | a stale `vari` that points into reused arena memory |
| ThreadSanitizer | `test_capi`, `test_pool`, `test_multichain`, `test_wa_parallel`, `test_wa_parallel_abi` and `test_wa_parallel_switches` | no diagnostics | post-merge except release tags | the other tests |
| typed producer | portable MIR from the typed producer for three models | the legacy MIR of the same source | PR and post-merge, Linux x86-64 | other models |
| bundled compiler | `r/inst/js/stanc.js`, and the compiler loaded in a browser worker, in webR and through V8 in R | a fresh build, byte for byte; expected compile results | PR | |
| Windows compiler | the Windows build of the compiler | expected results in `tests/test_windows_compiler.R`; no integer folded past int32 | PR | |
| R ABI | the ABI constants in the runtime and in the R bridge | each other | PR and pushes that touch the R paths | |
| `R CMD check` matrix | the R package without a runtime, on 4 OS and R combinations, warnings as errors | R's own checks | post-merge | sampling, which skips without a runtime |
| packaging guards | glibc symbol versions, the macOS minimum version, the x86-64 instruction set of the Linux library | the wheel tag and the baseline ISA | every build | |
| signature watch | the signatures of upstream stanc3 | the signatures in the conformance baseline | nightly; it reports and does not fail | |
| Safari 17 bundle check | the js_of_ocaml bundles | no line ending in `static` | PR | |
| docs and formatting | stamped numbers, C/C++ formatting | their artifacts ([`tools/gen_docs.py --check`](tools/gen_docs.py)), the pinned formatter | every PR | |
| red-main issue | whether any post-merge job of `wheels.yml` failed | none: it opens, updates or closes one GitHub issue, "main is red: wheels" | after each push to `main`, nightly run and manual run on `main` | a run in which a job is cancelled |

<a id="installed-clients"></a>
The wheel jobs run `tests/test_python.py`, `tests/test_bridgestan_embed.py`
and six Laplace whitening and annotation tests on the installed wheel. The
Linux R job runs `R CMD check` with the runtime and then
[`tools/test_r_ecosystem.R`](tools/test_r_ecosystem.R), whose testthat files
(bayesplot, bridgesampling, loo, posterior, stanfit and the registration of
the backend) must pass with no skip, failure, error or warning. The macOS and
Windows R jobs run only that script.

Three checks fail on time and compare no numbers: the MIR wire cost,
`test_prep_scaling` (preparation must grow less than 20-fold when a model
grows 8-fold) and `test_prep_deep_expression_time` (a 20,000-deep expression
must prepare in under 3 seconds). Release tags also check the version
strings, the runtime tarballs and the PyPI description.

The tables leave out the tests of CI's own scripts (`test_ci_policy`,
`test_dev_setup` and similar), two measurements that gate nothing (R time to
first posterior, and a webR canary that only warns), and four harnesses that
CI never runs: `fn_sweep.py`, `ode_sweep.py`, `transform_sweep.py` and
`collapse_census.py` in [`harnesses`](harnesses).

The sections below say more about the checks that need it.

### Recording and replaying the references

[`tools/verify_sample.py`](tools/verify_sample.py) records the references with
CmdStan's own driver ([`tools/ref_driver.cpp`](tools/ref_driver.cpp)), with
`propto=true` and the Jacobian. Both drivers seed Stan's RNG the same way, so
generated quantities that draw random numbers compare too.
[`tools/verify_refs.py`](tools/verify_refs.py) replays them and needs no
CmdStan. The replay fails on a missing point, on changed model or data files,
and on a reference that was recorded against a CmdStan, Stan or Stan Math
other than the pinned one.

Linux x86-64 has its own recording of the brms models, and a second one for
GCC, because a CmdStan gradient can differ by hundreds of ULP between
platforms and compilers. The replay picks the recording from the platform and
compiler of the binary under test, before it evaluates anything.

Three lists in `tools/verify_refs.py` can excuse a failure: `KNOWN_GAPS` (a
model that stanli refuses), `QUARANTINED` (a point with an open bug) and
`CORPUS_INTERPRETED` (a model that runs through the interpreter). All three
are empty. An entry whose model starts to pass fails the run until someone
deletes the entry.

### Unit tests

A kernel is the runtime's implementation of one operation. Each kernel test
builds the call that CmdStan's generated C++ would make with
`stan::math::var`, and compares the value and every gradient component.
[`tests/expect.hpp`](tests/expect.hpp) has the assertions. `expect_eq` compares
every bit, so `+0` and `-0` differ. `expect_ulp` takes its budget at the call,
where a reader can see it. Most comparisons are `expect_eq`. The largest
kernel budget is 16 ULP, for the adjoints of the matrix solves, and the
largest lowering budget is 40 ULP, where a chain of matrix pullbacks adds in a
different order.

`sum()` and `mean()` of more than 32 values add their terms with compensation,
and the sums that stanli builds from a loop's terms add them pairwise.
[`tests/test_sum_accuracy.cpp`](tests/test_sum_accuracy.cpp) holds both to the
exact sum of the same doubles. Stan Math adds in order, so these are the cause
of most ledger entries where stanli is closer to the true value.

Seven common densities run through fused kernels. A default CMake build keeps
the Stan Math path that they replace, and
[`tests/test_density_fused.cpp`](tests/test_density_fused.cpp) compares the two
over every activity mask, shape, edge value and rejection message. Normal and
cauchy may differ by 2 ULP, and the other five must match in every bit.

[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) compares gradients
before and after the graph passes on 400 random graphs. A regression test must
fail against the source from before the fix, for the expected reason, before
it counts.

### Comparing stanli's execution paths

Graph lowering, the MIR interpreter and the register program each implement
Stan's operations separately, so one can be wrong while another is right.
[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles a model once for each
of 19 configurations and compares the results at three points. Every bit must
match, unless [`tests/cross_path_ledger.json`](tests/cross_path_ledger.json)
lists the difference with a cause and a bound in ULP. An entry that excuses
nothing in a full run fails that run.

Most of the ledger's <!--gen:testing_cross_entries-->58<!--/gen--> entries are
sums that the region map and the lane passes regroup, by 1 to 10 ULP. Two
`cm_rdm` entries allow 2,526 ULP on Windows for a rounding difference there.
Another <!--gen:testing_cross_unbounded-->10<!--/gen--> entries allow any
distance, for one open bug: a gradient entry of `higher_order_all_contexts`
and of `ode_adjoint_region` was far off on Windows x86-64 in some runs, and
the cause is not known. Of those, <!--gen:testing_cross_unbounded_everywhere-->1<!--/gen-->
names no platform, so it allows any distance on every platform.

The matrix runs over every fixture and over the 22 cogmod models at 13 points.
It needs developer switches, so it runs only in a build that has them (see
[Developer switches](#developer-switches)).

<a id="mir-loop-vectorization-measurement"></a>
After a change to a pass, run `stanli_check --cross --cross-corpus
--cross-rel-tol 1e-11` over posteriordb by hand.
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) has the steps.

### Fast mode

Fast mode (`fast_math`, see [docs/fast-mode.md](docs/fast-mode.md)) may reorder
sums and replace arithmetic over observations with closed forms, so the 10 ULP
rule does not apply to it. `tools/verify_refs.py --fast-math` replays the same
points against the same CmdStan references. At each point:

- The log density must be within 1e-12 relative error.
- The largest gradient difference, divided by the largest gradient entry (or
  by 1 if that is larger), must be below 1e-12. That is about 4,500 ULP of the
  largest entry.
- Rejections and non-finite values must match, and `write_array` values keep
  the 1e-9 limit.

A point may exceed 1e-12 only when an 80-digit reference shows fast mode about
as close to the true value as CmdStan. `FAST_HP_CHECKED` lists one such model,
`epinow2_estimate_infections`, with its limit of 1e-10. The ill-conditioned
models and the known mismatches keep the limits that the table gives them.
[`tests/test_collapse.cpp`](tests/test_collapse.cpp) compares every graph that
fast mode rewrites with the graph it came from. Measured accuracy is in
[docs/fast-mode.md](docs/fast-mode.md#how-accurate).

### Samplers and other algorithms

A gradient at a fixed point cannot show a wrong sampler setting.

- [`tests/test_sampler_parity.cpp`](tests/test_sampler_parity.cpp) runs one
  executor through stanli's NUTS driver and through Stan's own `hmc_nuts_*`
  services, which CmdStan calls. Every saved row must match in every bit. The
  cases cover the three metrics, adaptation on and off, a supplied metric and
  step size, thinning, saved warmup and chain ids.
- [`tests/test_capi_algorithms.cpp`](tests/test_capi_algorithms.cpp) runs
  optimize, Laplace, Pathfinder, ADVI and fixed_param on a correlated
  Gaussian. The mode must be within 1e-4, and the others must recover the
  means and standard deviations within five standard errors.
- [`tools/verify_posteriors.py`](tools/verify_posteriors.py) samples the 39
  posteriordb models that have reference draws, with 4 chains of 1000
  iterations. A model fails when any mean or standard deviation is more than 4
  Monte Carlo standard errors from the reference. It runs by hand.
- [`tools/sampler_trace.py`](tools/sampler_trace.py) compares leapfrog counts,
  tree depth, step size and divergences with a CmdStan run of the same model
  and seed. It finds large configuration errors, and runs by hand.
- [`harnesses/brms_compare_backends.R`](harnesses/brms_compare_backends.R)
  fits each brms formula with the `stanli` and `cmdstanr` backends and
  compares posterior means
  ([latest recording](output/brms-end-to-end/README.md)). It runs by hand.

The Python and R tests check only that each binding passes arguments, results
and errors through.

<a id="testing-stan-language-coverage"></a>

### Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py) reads every
signature that the pinned compiler knows, and a catalog of language
constructs. For each one it generates a small model, evaluates it with stanli
and with BridgeStan built from the same pins, and compares the log density and
gradient. [docs/coverage.md](docs/coverage.md) summarizes the result by
function family. A case that stops verifying fails the nightly run.

stanc3's own integration tests cover syntax and type combinations that no real
model uses. [`tools/check_stanc3_good.py`](tools/check_stanc3_good.py)
compares the `good` models with recorded CmdStan values and messages, and
[`tools/check_stanc3_bad.py`](tools/check_stanc3_bad.py) compares the error
text of the `bad` models with stanc3's expected output. Each has a ledger, and
a model fails if it does better or worse than its entry
([`tests/stanc3_good/README.md`](tests/stanc3_good/README.md)).

The native, JavaScript and Windows compilers must emit the same compact MIR
bytes for the [compiler fixtures](tests/compiler/README.md).

### Developer switches

About 60 `STANLI_*` environment variables turn a pass off or force an engine.
Shipped builds compile them out (`STANLI_DEBUG_SWITCHES=OFF`). A test that
needs one carries the CTest label `debug_switches`, and skips itself or is
disabled in a shipped build. The cross-path matrix and eight other tests are
disabled there. They run where a build has the switches: a second build on
Linux x86-64 for pull requests, and the AddressSanitizer and Windows ARM64
jobs after a merge. They do not run on macOS in CI.
[docs/hacking.md](docs/hacking.md#tests-that-need-developer-switches) lists
them.

## Known limits

- **The 10 ULP rule is enforced on one platform, after merge.** The ledger
  has a section for macOS arm64 only. A pull request can move a value by more
  than 10 ULP and merge, as long as the value stays within 1e-9 and is not in
  a brms model or the `fixtures` corpus. The macOS run after the merge then
  fails and opens the red-main issue. Recording a ledger section on Linux
  x86-64 would close this gap, and would let the separate brms limit go.
- **Open ledger entries.** CmdStan is closer to the true value in <!--gen:testing_entries_cmdstan-->7<!--/gen-->
  entries, and another <!--gen:testing_entries_unknown-->47<!--/gen--> are
  undecided. For the undecided ones, the 80-digit evaluator covers only the
  log density and the gradient, gives only directional derivatives for large
  models, or lacks a function such as the wiener density. Search the table
  for "CmdStan closer" and "undecided".
- **Three points for each model.** Agreement at three points does not prove
  agreement over the parameter space. The sampler checks cover a little of
  that gap.
- **Models with a known mismatch.** Two gradient entries of `kronecker_gp`
  flow through `eigenvectors_sym` of a covariance whose smallest eigenvalue
  gap is 6.5e-17, and differ from CmdStan by 0.7%. Three gradient entries of
  `stanguide_ode_adjoint` come from an adjoint ODE solve at a tolerance of
  1e-6, and differ by up to 4e-6. Solved at 1e-11, the two engines agree to
  1e-10.
- **Models with no comparison.** Both engines reject every point of the
  posteriordb model `sir` and the brms model `s2_invgaussian`.
  Another <!--gen:testing_models_no_wa-->6<!--/gen--> models have no recorded
  `write_array` values ("not compared" in the table). For those, stanli must
  produce a row, and nothing checks its values.
- **Windows** runs CTest, with the local corpora, and never the main corpus
  replay. One cross-path bug is open there: a gradient entry of
  `higher_order_all_contexts` and of `ode_adjoint_region` was far off on
  Windows x86-64 in some runs. One of the ledger entries for it names no
  platform, so that comparison of `higher_order_all_contexts` is unchecked
  everywhere.
- **WebAssembly.** CI builds the WebAssembly runtime and runs Eight Schools
  through it. The corpus replay through it runs by hand, without
  `write_array`.
- **Fast mode** is replayed on Linux x86-64 only.
- **The `cmdstan_replay` references** carry no CmdStan revision, so a change
  of pin does not fail that corpus.
- **Not tested at all:** the R interrupt path. The data-loop memo has one
  test, [`tests/test_data_loop_memo.py`](tests/test_data_loop_memo.py), and no
  cross-path fixture.
- **The order of a sum is not fixed across releases.** A release can move a
  value by its last bits within the rule. The release notes give the largest
  distance measured.
