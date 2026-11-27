# Why should I trust this?

stanli reimplements the Stan runtime. We test it against CmdStan at the
versions pinned in [`deps/pins.env`](deps/pins.env). This page has four parts:

- [The rule](#the-rule) that we hold every model to, and where CI enforces it
- [Every model](#every-model), with how accurate each engine is on it
- [The checks](#the-checks) that CI and we run
- [Known limits](#known-limits)

<a id="numerical-policy"></a>

## The rule

Each model has three fixed test points in unconstrained space. At each point
stanli computes the log density, every gradient entry and every value that a
draw writes. A draw writes the constrained parameters, the transformed
parameters and the generated quantities. The code calls these values
`write_array`, and this page calls them outputs.

**Each of those numbers must be within 10 ULP of the number that CmdStan
computes. If a number is farther away, the run fails, unless
[`tests/ulp_ledger.json`](tests/ulp_ledger.json) lists that value with the
distance that we measured and the cause.**

This rule applies to every model in every corpus. Some details:

- A ULP is one step between adjacent doubles.
- stanli and CmdStan call the same Stan Math with the same floating-point
  flags. Most differences come from the order in which each engine adds up a
  sum.
- If CmdStan rejects a point, stanli must reject it too. If CmdStan returns
  NaN or an infinity, stanli must return the same.
- stanli does not have to match CmdStan in every bit.
- The bound in a ledger entry is the largest distance that we measured over
  the three points. `tools/verify_refs.py` can lower a bound. A person has to
  raise a bound or add an entry by hand, in a commit that gives the cause.
- Each ledger entry says which engine is closer to the true value at the point
  where stanli is farthest from CmdStan. It says `stanli`, `cmdstan` or
  `unknown`. We get the true value from an 80-digit evaluation of the model
  ([`tools/hp`](tools/hp/README.md)).
- Most changes should move a result by 2 ULP or less. A change that adds up a
  sum in a different order can move a result by up to 10 ULP, and its commit
  message should say that it does. Reviewers check this. CI does not.

<a id="true-values"></a>

**Some ledger entries also store the true value at each point. For those
values, the run checks stanli against the true value and does not check the
distance from CmdStan. stanli must be within 10 ULP of the true value, or
within the entry's own bound if that is larger.**

- The bound of a true value is the largest distance of stanli from it that we
  measured over the three points. `tools/verify_refs.py` can lower it, but
  only on the platform that measured it. A person has to raise it by hand.
- A change that moves stanli closer to the true value can move it away from
  CmdStan. With a true value, such a change passes.
- Only an entry whose `closer` is `stanli` can store true values. A gradient
  entry also needs a full finite-difference gradient from the 80-digit
  evaluation at every point that CmdStan accepts. The distances that the
  entry's reason gives must agree with those that the tool measures again.
- <!--gen:testing_truth_entries-->107<!--/gen--> entries of <!--gen:testing_truth_models-->64<!--/gen--> models store true
  values. The bounds of <!--gen:testing_truth_over-->20<!--/gen--> of them are over 10.
- `tools/hp/ledger_accuracy.py --write-true-values` writes them
  ([`tools/hp`](tools/hp/README.md#the-ulp-ledger)).

<a id="the-ulp-gate"></a>

### How we count the distance

Call the CmdStan value `a` and the stanli value `b`. The distance is

    min(doubles between a and b, ceil(|a - b| / ulp(S)))

`S` is the largest CmdStan value, in magnitude, in the group that `a` belongs
to. For a gradient entry the group is the whole gradient. For an output it is
the variable that the output is an element of. For the log density `S` is
`|a|`.

We need the second term for gradient entries that cancel to nearly zero. When
one term of such an entry rounds differently, the entry can move by millions
of its own ULP. So we also measure it against the largest gradient entry and
take the smaller count. `S` uses only CmdStan's values, so a wrong stanli
value cannot make `S` larger.

For a value with a true value `t`, the distance is `|b - t| / ulp(S)`. The
ledger stores `S` with `t`. We took `S` from the macOS arm64 recording of
CmdStan, so the distance does not depend on the CmdStan recording of the
platform that runs the check.

### Where CI enforces the rule

CI enforces the 10 ULP rule only on macOS arm64, because the ledger has
measurements only from that platform. macOS arm64 runs after a merge to
`main`. Pull requests run on Linux x86-64, where CI enforces two other limits:

- The relative error `|a - b| / max(|a|, |b|, 1)` of every value must be below
  1e-9. Every platform enforces this limit.
- Every value of the <!--gen:testing_brms_limited-->124<!--/gen--> brms models and of the
  `fixtures` corpus must be within 10 ULP of CmdStan. This limit counts plain
  ULP for each element and does not use the ledger. Linux x86-64 has its own
  CmdStan recording of the brms models for it.

| limit | applies to | macOS arm64 | Linux x86-64 | other platforms |
| --- | --- | --- | --- | --- |
| 10 ULP or the ledger bound, from the true value | the values whose ledger entries store true values | yes | yes | yes |
| 10 ULP or the ledger bound, from CmdStan | every other value of every model | yes | no | no |
| 10 ULP for each element | brms models and the `fixtures` corpus | yes | yes | `fixtures` only |
| 1e-9 relative error | every value of every model | yes | yes | yes |
| rejections and non-finite values match | every point | yes | yes | yes |

The check against the true values runs on every platform, because the true
values do not depend on the platform. The three points are the same
parameter values on every platform. The replay in WebAssembly and the replay
in fast mode do not run this check.

Windows runs the `fixtures` corpus and the other local corpora in CTest. It
does not run the main corpus.

### The limit of each kind of comparison

This table gives the limit for each kind of numerical comparison and the file
that lists its exceptions.

| comparison | limit | exceptions |
| --- | --- | --- |
| a model's values against CmdStan | 10 ULP | [`tests/ulp_ledger.json`](tests/ulp_ledger.json) |
| a model's values against their true values, where the ledger stores them | 10 ULP | the bounds of `true_values` in [`tests/ulp_ledger.json`](tests/ulp_ledger.json) |
| the same, on every platform | 1e-9 relative error | `ILL_CONDITIONED` and `MISMATCH_OUTPUTS` in [`tools/verify_refs.py`](tools/verify_refs.py). They name <!--gen:testing_floored_models-->6<!--/gen--> models, which the table below marks. |
| brms models against CmdStan on the same platform | 10 ULP for each element | `ULP_LIMITS` and `GRADIENT_AGAINST_LARGEST` in `tools/verify_refs.py`. The table below marks the models. |
| the `fixtures` corpus against CmdStan | 10 ULP for each element | `VALUE_ULP_LIMITS` (2 models), `GRADIENT_AGAINST_LARGEST_OFF_PLATFORM` (3 models) and `WRITE_ARRAY_AGAINST_LARGEST_OFF_PLATFORM` (1 model) in `tools/verify_refs.py` |
| a model in [fast mode](#fast-mode) against CmdStan | 1e-12 relative error | `FAST_HP_CHECKED` in `tools/verify_refs.py` (1 model) |
| stanli's execution paths against each other | every bit | [`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) |
| one kernel against the Stan Math call that it replaces | every bit | the budget written at the assertion |
| a generated signature case against BridgeStan | 10 ULP | the per-case entries of the conformance baseline |
| stanli's NUTS against Stan's NUTS services | every bit | none |

<a id="comparison-with-cmdstan-on-complete-models"></a>

## Every model

The main corpus has <!--gen:testing_models-->415<!--/gen--> models. We recorded CmdStan's
answers at 3 points for each model. That makes <!--gen:testing_points-->1245<!--/gen--> points
and <!--gen:testing_values-->1,504,551<!--/gen--> values. CmdStan rejects <!--gen:testing_rejected_points-->6<!--/gen-->
of the points.

In <!--gen:testing_models_within-->326<!--/gen--> models, every value is within 10 ULP of
CmdStan. The other <!--gen:testing_models_over-->89<!--/gen--> models have <!--gen:testing_entries-->197<!--/gen-->
ledger entries. stanli is at least as close to the true value as CmdStan in <!--gen:testing_entries_stanli-->150<!--/gen-->
of those entries. CmdStan is closer in <!--gen:testing_entries_cmdstan-->7<!--/gen-->. We
cannot tell for the other <!--gen:testing_entries_unknown-->40<!--/gen-->. Of the entries where
stanli is closer, <!--gen:testing_truth_entries-->107<!--/gen--> store true values, and the run
checks those values against the [true values](#true-values).

<!--gen:testing_collections-->
| source | models | models with a value over 10 ULP |
| --- | ---: | ---: |
| posteriordb | 119 | 43 |
| educational | 13 | 1 |
| brms | 124 | 4 |
| rethinking | 62 | 14 |
| cogmod | 22 | 12 |
| ctsem | 3 | 1 |
| examplemodels | 23 | 6 |
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
| regressions | 14 | 0 |
| all | 415 | 89 |
<!--/gen-->

The models come from posteriordb, brms formulas, the code of Statistical
Rethinking, cogmod's brms families, the Aalto Bayesian data analysis course
(`educational`), ctsem's state-space template, stan-dev/example-models, the
Stan User's Guide and 13 CRAN packages. The `regressions` models are programs
that stanli once failed to compile.
[`tools/corpus_inventory.py`](tools/corpus_inventory.py) lists them all.

### How accurate each engine is

We also evaluated each model with 80 digits of precision
([`tools/hp`](tools/hp/README.md)). That gives the true value of the log
density, of each gradient entry and of each output. For each model, the big
table gives the mean distance of CmdStan's values from the true values, and
the mean distance of stanli's values, in ULP. The mean is over all values at
all three points.

This table counts the models where one engine has the smaller mean:

| values | models scored | stanli closer | CmdStan closer | the same |
| --- | ---: | ---: | ---: | ---: |
| log density | <!--gen:testing_scored_lp-->370<!--/gen--> | <!--gen:testing_scored_lp_stanli-->92<!--/gen--> | <!--gen:testing_scored_lp_cmdstan-->68<!--/gen--> | <!--gen:testing_scored_lp_same-->210<!--/gen--> |
| gradient | <!--gen:testing_scored_grad-->337<!--/gen--> | <!--gen:testing_scored_grad_stanli-->217<!--/gen--> | <!--gen:testing_scored_grad_cmdstan-->57<!--/gen--> | <!--gen:testing_scored_grad_same-->63<!--/gen--> |
| outputs | <!--gen:testing_scored_wa-->367<!--/gen--> | <!--gen:testing_scored_wa_stanli-->100<!--/gen--> | <!--gen:testing_scored_wa_cmdstan-->16<!--/gen--> | <!--gen:testing_scored_wa_same-->251<!--/gen--> |

We scored <!--gen:testing_scored_values-->765,761<!--/gen--> values in total. stanli is closer to
the true value in <!--gen:testing_values_stanli_closer-->16,225<!--/gen--> of them and CmdStan is
closer in <!--gen:testing_values_cmdstan_closer-->16,631<!--/gen-->. The two engines are the same
distance away in the rest.

We measured these numbers on macOS arm64 on <!--gen:testing_accuracy_date-->2026-11-25<!--/gen-->, at
commit <!--gen:testing_accuracy_source-->`c2f6db4b`<!--/gen-->. CI does not measure them again.

### How to read the table

- **parameters** is the number of gradient entries.
- **outputs** is the number of `write_array` values at one point.
- The **lp**, **gradient** and **outputs** columns come in pairs. The first
  column of a pair is CmdStan's mean distance from the true value, in ULP. The
  second is stanli's. A smaller number is more accurate. We count the distance
  with the same `S` as in the rule.
- If both columns of a pair are large, double arithmetic loses digits at those
  points in both engines.
- An empty cell means that we have no 80-digit value. The 80-digit evaluator
  gives no gradient for a model with more than 300 parameters. It does not
  implement some functions, such as the ODE solvers and the wiener density. It
  skips an output that depends on a random draw.
- **limit** is how far stanli may be from CmdStan on macOS arm64, in ULP. `10`
  is the rule. Any other entry is the largest ledger bound for that kind of
  value. The ledger gives the cause of each bound. A limit marked "from the
  true value" is for the values whose entries store true values. It says how
  far stanli may be from the true value, on every platform.
- **also** lists other limits on the model. "brms limit 10" is the limit for
  each element from the table above. Some brms models say "gradient against
  its largest entry". For those, we count a gradient entry in ULP of the
  largest gradient entry, because the gradient entries of mean-centred
  predictors cancel. An ill-conditioned model and a known mismatch have their
  own relative-error limit for the values that the cell names.

`python3 tools/gen_docs.py` writes the table. It reads the CmdStan references,
the ledger, `tools/verify_refs.py` and the recorded measurements in
[`accuracy.json`](docs/internal/artifacts/accuracy.json). CI fails when the
table and those files disagree.

<!--gen:testing_corpus_table-->
| model | source | parameters | outputs | lp: CmdStan | lp: stanli | gradient: CmdStan | gradient: stanli | outputs: CmdStan | outputs: stanli | limit | also |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `2pl_latent_reg_irt` | posteriordb | 531 | 549 | 226 | 226 |  |  | 0.01 | 0.01 | gradient 29 |  |
| `accel_gp` | posteriordb | 66 | 72 | 1.51 | 1.51 | 0.06 | 0.02 | 0.03 | 0.03 | 10 |  |
| `accel_splines` | posteriordb | 82 | 160 | 0.58 | 0.58 | 0.09 | 0.07 | 0.05 | 0.05 | 10 |  |
| `arK` | posteriordb | 7 | 7 | 3.76 | 1.82 | 0.56 | 0.18 | 0.03 | 0.03 | 10 |  |
| `arma11` | posteriordb | 4 | 4 | 0.60 | 0.60 | 0.60 | 0.60 | 0.07 | 0.07 | 10 |  |
| `blr` | posteriordb | 6 | 6 | 0.90 | 0.90 | 0.65 | 0.65 | 0.05 | 0.05 | 10 |  |
| `bones_model` | posteriordb | 13 | 13 | 0.64 | 0.64 | 0.58 | 0.59 | <0.01 | <0.01 | 10 |  |
| `bym2_offset_only` | posteriordb | 3845 | 9610 | 2.18 | 2.63 |  |  | 0.29 | 0.29 | 10 |  |
| `covid19imperial_v2` | posteriordb | 51 | 8457 | 0.56 | 0.99 | 0.13 | 0.05 | 0.14 | 0.15 | 10 |  |
| `covid19imperial_v3` | posteriordb | 51 | 8457 | 0.56 | 0.99 | 0.13 | 0.05 | 0.14 | 0.15 | 10 |  |
| `diamonds` | posteriordb | 26 | 27 | 1.63 | 1.96 | 0.60 | 0.10 | 3.58 | 0.23 | outputs 12 from the true value |  |
| `dogs` | posteriordb | 3 | 2253 | 25.1 | 1.51 | 4.55 | 0.21 | 0.03 | 0.03 | lp 10 from the true value, gradient 14, gradient 10 from the true value |  |
| `dogs_hierarchical` | posteriordb | 2 | 752 | 9.39 | 4.28 | 3.52 | 1.54 | 0.27 | 0.27 | lp 10 from the true value |  |
| `dogs_log` | posteriordb | 2 | 2252 |  |  |  |  |  |  | 10 |  |
| `dogs_nonhierarchical` | posteriordb | 65 | 946 | 10.1 | 1.99 | 0.04 | 0.04 | 0.12 | 0.12 | lp 10 from the true value |  |
| `dugongs_model` | posteriordb | 4 | 6 | 0.18 | 0.51 | 0.43 | 0.34 | 0.17 | 0.17 | 10 |  |
| `earn_height` | posteriordb | 3 | 3 | 3.33 | 3.33 | 0.91 | 0.91 | 0.05 | 0.05 | 10 |  |
| `eight_schools_centered` | posteriordb | 10 | 10 | 0.70 | 0.70 | 0.06 | 0.06 | 0.01 | 0.01 | 10 |  |
| `eight_schools_noncentered` | posteriordb | 10 | 18 | 0.56 | 0.56 | 0.10 | 0.10 | 0.13 | 0.13 | 10 |  |
| `election88_full` | posteriordb | 90 | 11656 | 255 | 255 | 1.96 | 0.04 | 0.19 | 0.19 | gradient 10 from the true value |  |
| `garch11` | posteriordb | 4 | 4 | 0.45 | 0.45 | 3.02 | 3.02 | 0.17 | 0.17 | 10 |  |
| `GLM_Binomial_model` | posteriordb | 3 | 83 | 2.52 | 2.52 | 0.46 | 0.16 | 0.25 | 0.26 | 10 |  |
| `GLM_Poisson_model` | posteriordb | 4 | 84 | 14.5 | 14.7 | 0.51 | 0.54 | 6.73 | 6.76 | 10 |  |
| `GLMM1_model` | posteriordb | 237 | 2395 | 16.9 | 0.52 | 0.07 | 0.07 | <0.01 | <0.01 | lp 10 from the true value |  |
| `GLMM_Poisson_model` | posteriordb | 45 | 125 | 13.2 | 12.8 | 1.03 | 1.05 | 1.06 | 1.06 | 10 |  |
| `gp_pois_regr` | posteriordb | 13 | 24 | 0.73 | 0.73 | 0.21 | 0.21 | 0.13 | 0.13 | 10 |  |
| `gp_regr` | posteriordb | 3 | 3 | 0.80 | 0.80 | 0.69 | 0.69 | 0.17 | 0.17 | 10 |  |
| `gpcm_latent_reg_irt` | posteriordb | 530 | 550 | 56.2 | 0.94 |  |  | 0.02 | 0.02 | lp 10 from the true value, gradient 55 |  |
| `grsm_latent_reg_irt` | posteriordb | 408 | 418 | 182 | 1.55 |  |  | <0.01 | <0.01 | lp 493, gradient 23 |  |
| `hier_2pl` | posteriordb | 669 | 804 | 602 | 602 |  |  | 0.01 | 0.01 | gradient 28 |  |
| `hierarchical_gp` | posteriordb | 933 | 3279 | 0.33 | 0.66 |  |  | 0.29 | 0.29 | 10 |  |
| `hmm_drive_0` | posteriordb | 6 | 429 | 32.6 | 32.9 | 10,568 | 10,568 | 0.09 | 0.09 | 10 |  |
| `hmm_drive_1` | posteriordb | 6 | 429 | 36.7 | 37.0 | 55,945 | 55,945 | 0.09 | 0.09 | 10 |  |
| `hmm_example` | posteriordb | 4 | 111 | 10.4 | 11.1 | 423 | 423 | 0.11 | 0.11 | 10 |  |
| `hmm_gaussian` | posteriordb | 14 | 9519 | 18.1 | 17.7 | 10,342 | 10,342 | 961 | 961 | 10 |  |
| `iohmm_reg` | posteriordb | 29 | 18031 | 4.43 | 4.43 | 14,117 | 14,117 | 41.6 | 41.6 | 10 |  |
| `irt_2pl` | posteriordb | 144 | 144 | 2.23 | 2.23 | 0.10 | 0.04 | 0.03 | 0.03 | 10 |  |
| `kidscore_interaction` | posteriordb | 5 | 5 | 1.04 | 1.04 | 2.03 | 0.35 | 0.05 | 0.05 | 10 |  |
| `kidscore_interaction_c` | posteriordb | 5 | 5 | 0.89 | 0.89 | 0.20 | 0.13 | 0.05 | 0.05 | 10 |  |
| `kidscore_interaction_c2` | posteriordb | 5 | 5 | 0.37 | 0.37 | 0.10 | 0.08 | 0.05 | 0.05 | 10 |  |
| `kidscore_interaction_z` | posteriordb | 5 | 5 | 0.38 | 0.38 | 0.07 | 0.07 | 0.05 | 0.05 | 10 |  |
| `kidscore_mom_work` | posteriordb | 5 | 5 | 0.45 | 0.45 | 0.08 | 0.07 | 0.05 | 0.05 | 10 |  |
| `kidscore_momhs` | posteriordb | 3 | 3 | 0.26 | 0.26 | 0.28 | 0.19 | 0.05 | 0.05 | 10 |  |
| `kidscore_momhsiq` | posteriordb | 4 | 4 | 0.92 | 0.92 | 1.36 | 0.43 | 0.06 | 0.06 | 10 |  |
| `kidscore_momiq` | posteriordb | 3 | 3 | 0.27 | 0.27 | 1.58 | 0.24 | 0.05 | 0.05 | 10 |  |
| `kilpisjarvi` | posteriordb | 3 | 3 | 0.74 | 0.74 | 0.29 | 0.29 | 0.05 | 0.05 | 10 |  |
| `kronecker_gp` | posteriordb | 438 | 0 | 5.35 | 6.68 |  |  |  |  | gradient 2.6e12 | known mismatch in grad[1], grad[2], which have their own relative limit |
| `ldaK2` | posteriordb | 7 | 12 | 105 | 0.68 | 113 | 3.08 | 0.32 | 0.32 | lp 10 from the true value, gradient 13 from the true value |  |
| `ldaK5` | posteriordb | 7714 | 7780 | 290 | 2.18 |  |  | 3.11 | 3.11 | lp 10 from the true value, gradient 38 |  |
| `log10earn_height` | posteriordb | 3 | 3 | 0.86 | 0.86 | 3.43 | 0.32 | 0.05 | 0.05 | gradient 10 from the true value |  |
| `logearn_height` | posteriordb | 3 | 3 | 1.31 | 1.31 | 3.70 | 0.43 | 0.05 | 0.05 | gradient 10 from the true value |  |
| `logearn_height_male` | posteriordb | 4 | 4 | 1.51 | 1.51 | 1.47 | 0.18 | 0.06 | 0.06 | 10 |  |
| `logearn_interaction` | posteriordb | 5 | 5 | 2.88 | 2.88 | 2.89 | 0.47 | 0.05 | 0.05 | gradient 10 from the true value |  |
| `logearn_interaction_z` | posteriordb | 5 | 5 | 2.70 | 2.70 | 0.61 | 0.47 | 0.05 | 0.05 | 10 |  |
| `logearn_logheight_male` | posteriordb | 4 | 4 | 2.09 | 2.09 | 1.09 | 0.63 | 0.06 | 0.06 | 10 |  |
| `logistic_regression_rhs` | posteriordb | 3075 | 4719 | 3,219 | 3,219 |  |  | 27.2 | 27.2 | 10 |  |
| `logmesquite` | posteriordb | 8 | 8 | 1.12 | 1.12 | 0.22 | 0.19 | <0.01 | <0.01 | 10 |  |
| `logmesquite_logva` | posteriordb | 5 | 5 | 0.50 | 0.50 | 0.20 | 0.16 | 0.05 | 0.05 | 10 |  |
| `logmesquite_logvas` | posteriordb | 8 | 8 | 0.39 | 0.39 | 0.14 | 0.12 | <0.01 | <0.01 | 10 |  |
| `logmesquite_logvash` | posteriordb | 7 | 7 | 0.35 | 0.35 | 0.14 | 0.12 | 0.03 | 0.03 | 10 |  |
| `logmesquite_logvolume` | posteriordb | 3 | 3 | 0.93 | 0.93 | 0.37 | 0.39 | 0.05 | 0.05 | 10 |  |
| `losscurve_sislob` | posteriordb | 15 | 384 | 0.86 | 0.86 | 0.10 | 0.08 | 0.27 | 0.27 | 10 |  |
| `lotka_volterra` | posteriordb | 8 | 90 |  |  |  |  |  |  | 10 |  |
| `low_dim_gauss_mix` | posteriordb | 5 | 5 | 6.35 | 0.08 | 1.80 | 0.60 | 0.18 | 0.18 | lp 10 from the true value, gradient 10 from the true value |  |
| `low_dim_gauss_mix_collapse` | posteriordb | 5 | 5 | 7.00 | 0.33 | 1.52 | 0.56 | 0.13 | 0.13 | 10 |  |
| `lsat_model` | posteriordb | 1006 | 1012 | 55.7 | 55.7 |  |  | <0.01 | <0.01 | gradient 70 |  |
| `M0_model` | posteriordb | 2 | 4 | 49.0 | 0.29 | 73.2 | 3.78 | 0.69 | 0.69 | lp 10 from the true value, gradient 10 from the true value |  |
| `Mb_model` | posteriordb | 3 | 1596 | 27.8 | 0.47 | 21.0 | 6.74 | 0.12 | 0.12 | lp 69, gradient 22 from the true value |  |
| `mesquite` | posteriordb | 8 | 8 | 0.91 | 0.91 | 0.12 | 0.12 | <0.01 | <0.01 | 10 |  |
| `Mh_model` | posteriordb | 388 | 1159 | 24.2 | 2.43 |  |  | 3.01 | 3.01 | lp 10 from the true value, gradient 43 |  |
| `Mt_model` | posteriordb | 4 | 7 | 15.7 | 0.99 | 11.9 | 0.45 | 0.50 | 0.50 | lp 10 from the true value, gradient 10 from the true value |  |
| `Mtbh_model` | posteriordb | 154 | 1912 | 21.3 | 0.10 | 0.08 | 0.05 | 2.07 | 2.12 | lp 28, outputs 14 |  |
| `Mth_model` | posteriordb | 394 | 5044 | 19.1 | 0.23 |  |  | 1.17 | 1.23 | lp 24, gradient 28, outputs 14 |  |
| `multi_occupancy` | posteriordb | 106 | 312 | 14.2 | 2.44 | 0.02 | 0.01 | 0.10 | 0.10 | lp 10 from the true value |  |
| `nes` | posteriordb | 10 | 10 | 2.64 | 2.64 | 1.51 | 0.21 | 0.01 | 0.01 | 10 |  |
| `nes_logit_model` | posteriordb | 2 | 2 | 17.8 | 17.8 | 4.04 | 4.04 | <0.01 | <0.01 | 10 |  |
| `nn_rbm1bJ10` | posteriordb | 7951 | 7951 | 21.1 | 21.1 |  |  | <0.01 | <0.01 | 10 |  |
| `nn_rbm1bJ100` | posteriordb | 79411 | 79411 |  |  |  |  |  |  | lp 3,982 |  |
| `normal_mixture` | posteriordb | 3 | 3 | 4.11 | 0.26 | 7.44 | 6.98 | 0.14 | 0.14 | 10 |  |
| `normal_mixture_k` | posteriordb | 14 | 15 | 7.20 | 6.53 | 14.9 | 15.0 | 0.24 | 0.23 | gradient 15 |  |
| `one_comp_mm_elim_abs` | posteriordb | 4 | 44 |  |  |  |  |  |  | outputs 245 |  |
| `pilots` | posteriordb | 18 | 58 | 0.69 | 0.69 | 0.05 | 0.05 | 0.05 | 0.05 | 10 |  |
| `prophet` | posteriordb | 62 | 62 | 0.78 | 0.78 | 0.41 | 0.37 | <0.01 | <0.01 | 10 |  |
| `radon_county` | posteriordb | 389 | 389 | 0.38 | 0.38 |  |  | <0.01 | <0.01 | 10 |  |
| `radon_county_intercept` | posteriordb | 388 | 388 | 53.8 | 4.62 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 213 |  |
| `radon_hierarchical_intercept_centered` | posteriordb | 391 | 391 | 31.7 | 5.38 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 213 |  |
| `radon_hierarchical_intercept_noncentered` | posteriordb | 391 | 777 | 52.6 | 5.63 |  |  | 0.15 | 0.15 | lp 14 from the true value, gradient 213 |  |
| `radon_partially_pooled_centered` | posteriordb | 389 | 389 | 55.8 | 5.99 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 229 |  |
| `radon_partially_pooled_noncentered` | posteriordb | 389 | 775 | 43.3 | 6.41 |  |  | 0.07 | 0.07 | lp 14 from the true value, gradient 213 |  |
| `radon_pooled` | posteriordb | 3 | 3 | 136 | 7.77 | 54.3 | 5.75 | 0.05 | 0.05 | lp 14 from the true value, gradient 29 from the true value |  |
| `radon_variable_intercept_centered` | posteriordb | 390 | 390 | 73.1 | 6.76 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 213 |  |
| `radon_variable_intercept_noncentered` | posteriordb | 390 | 776 | 79.2 | 7.09 |  |  | 0.08 | 0.08 | lp 14 from the true value, gradient 213 |  |
| `radon_variable_intercept_slope_centered` | posteriordb | 777 | 777 | 55.9 | 8.66 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 213 |  |
| `radon_variable_intercept_slope_noncentered` | posteriordb | 777 | 1549 | 63.3 | 8.63 |  |  | 0.07 | 0.07 | lp 14 from the true value, gradient 213 |  |
| `radon_variable_slope_centered` | posteriordb | 390 | 390 | 78.7 | 12.0 |  |  | <0.01 | <0.01 | lp 14 from the true value, gradient 213 |  |
| `radon_variable_slope_noncentered` | posteriordb | 390 | 776 | 80.7 | 11.7 |  |  | 0.08 | 0.08 | lp 14 from the true value, gradient 213 |  |
| `Rate_1_model` | posteriordb | 1 | 1 | 0.53 | 0.53 | 24.2 | 24.2 | 0.33 | 0.33 | 10 |  |
| `Rate_2_model` | posteriordb | 2 | 3 | 0.23 | 0.23 | 0.47 | 0.47 | 11.8 | 11.8 | 10 |  |
| `Rate_3_model` | posteriordb | 1 | 1 | 0.57 | 0.57 | 1.87 | 1.87 | 0.33 | 0.33 | 10 |  |
| `Rate_4_model` | posteriordb | 2 | 4 | 0.62 | 0.95 | 0.31 | 0.31 | 0.27 | 0.27 | 10 |  |
| `Rate_5_model` | posteriordb | 1 | 3 | 0.64 | 0.64 | 22.5 | 22.5 | 0.33 | 0.33 | 10 |  |
| `rats_model` | posteriordb | 65 | 66 | 0.75 | 0.25 | 0.01 | <0.01 | 0.01 | 0.01 | 10 |  |
| `seeds_centered_model` | posteriordb | 26 | 47 | 0.54 | 0.54 | 0.23 | 0.22 | 0.09 | 0.09 | 10 |  |
| `seeds_model` | posteriordb | 26 | 27 | 0.22 | 0.22 | 0.20 | 0.20 | 0.02 | 0.02 | 10 |  |
| `seeds_stanified_model` | posteriordb | 26 | 26 | 0.38 | 0.38 | 0.21 | 0.20 | 0.01 | 0.01 | 10 |  |
| `sesame_one_pred_a` | posteriordb | 3 | 3 | 6.58 | 6.58 | 7.17 | 0.64 | 0.05 | 0.05 | gradient 10 from the true value |  |
| `soil_incubation` | posteriordb | 6 | 31 |  |  |  |  |  |  | 10 |  |
| `state_space_stochastic_level_stochastic_seasonal` | posteriordb | 389 | 581 | 6.01 | 4.34 |  |  | 0.18 | 0.18 | 10 |  |
| `surgical_model` | posteriordb | 14 | 28 | 0.37 | 0.37 | 0.09 | 0.09 | 0.12 | 0.12 | 10 |  |
| `Survey_model` | posteriordb | 1 | 1002 | 2.58 | 2.58 | 57.9 | 72.2 | 0.60 | 0.60 | gradient 69 |  |
| `wells_daae_c_model` | posteriordb | 6 | 6 | 1.82 | 1.49 | 1.40 | 0.97 | <0.01 | <0.01 | 10 |  |
| `wells_dae_c_model` | posteriordb | 5 | 5 | 1.37 | 1.71 | 1.56 | 1.46 | <0.01 | <0.01 | 10 |  |
| `wells_dae_inter_model` | posteriordb | 7 | 7 | 1.48 | 1.48 | 1.27 | 1.20 | <0.01 | <0.01 | 10 |  |
| `wells_dae_model` | posteriordb | 4 | 4 | 3.49 | 3.49 | 1.73 | 1.73 | <0.01 | <0.01 | 10 |  |
| `wells_dist` | posteriordb | 2 | 2 | 117 | 117 | 33.4 | 1.64 | <0.01 | <0.01 | gradient 10 from the true value |  |
| `wells_dist100_model` | posteriordb | 2 | 2 | 9.38 | 9.38 | 15.0 | 15.0 | <0.01 | <0.01 | 10 |  |
| `wells_dist100ars_model` | posteriordb | 3 | 3 | 3.25 | 3.25 | 2.16 | 2.16 | <0.01 | <0.01 | 10 |  |
| `wells_interaction_c_model` | posteriordb | 4 | 4 | 2.09 | 2.09 | 1.75 | 1.35 | <0.01 | <0.01 | 10 |  |
| `wells_interaction_model` | posteriordb | 4 | 4 | 1.60 | 1.60 | 1.17 | 1.17 | <0.01 | <0.01 | 10 |  |
| `aalto_bern` | educational | 1 | 12 | 0.33 | 0.33 | 0.84 | 0.84 | 0.43 | 0.43 | 10 |  |
| `aalto_binom` | educational | 1 | 1 | 0.33 | 0.33 | 1.17 | 1.17 | 0.33 | 0.33 | 10 |  |
| `aalto_binom2` | educational | 2 | 3 | 0.35 | 0.68 | 0.27 | 0.27 | 0.49 | 0.49 | 10 |  |
| `aalto_binomb` | educational | 1 | 2 | 0.52 | 0.52 | 0.91 | 0.91 | 0.20 | 0.20 | 10 |  |
| `aalto_gpareto` | educational | 2 | 30 | 0.57 | 0.57 | 2.66 | 1.51 | 0.33 | 0.33 | 10 |  |
| `aalto_grp_aov` | educational | 4 | 4 | 0.70 | 0.70 | 0.08 | 0.08 | 0.06 | 0.06 | 10 |  |
| `aalto_grp_prior_mean` | educational | 6 | 6 | 0.52 | 0.52 | 0.12 | 0.12 | 0.09 | 0.09 | 10 |  |
| `aalto_grp_prior_mean_var` | educational | 10 | 10 | 0.59 | 0.59 | 0.19 | 0.19 | 0.06 | 0.06 | 10 |  |
| `aalto_lin` | educational | 3 | 20 | 0.22 | 0.22 | 0.31 | 0.28 | 0.28 | 0.28 | 10 |  |
| `aalto_lin_std` | educational | 3 | 29 | 0.25 | 0.25 | 4.72 | 4.74 | 2.58 | 2.58 | 10 |  |
| `aalto_lin_std_t` | educational | 4 | 30 | 1.20 | 1.20 | 2.56 | 2.52 | 2.33 | 2.33 | 10 |  |
| `aalto_poisson_hurdle` | educational | 2 | 1002 | 13.8 | 0.64 | 13.2 | 0.39 | 0.26 | 0.26 | lp 10 from the true value, gradient 10 from the true value |  |
| `aalto_poisson_simple` | educational | 1 | 1001 | 1.08 | 1.08 | 1.63 | 1.63 | 0.29 | 0.29 | 10 |  |
| `i319_gauss_re` | brms | 66 | 127 | 0.79 | 0.79 | 0.02 | 0.02 | 0.03 | 0.03 | 10 | brms limit 10, gradient against its largest entry |
| `i319_negbin_fixed` | brms | 6 | 8 | 9.87 | 9.87 | 0.49 | 0.49 | 0.21 | 0.21 | 10 | brms limit 10, gradient against its largest entry |
| `i319_negbin_re` | brms | 66 | 127 | 9.29 | 9.29 | 0.08 | 0.07 | 0.02 | 0.02 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_fixed` | brms | 5 | 7 | 3.04 | 3.04 | 0.37 | 0.37 | 0.09 | 0.09 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_re` | brms | 65 | 126 | 2.10 | 2.10 | 0.06 | 0.04 | 0.10 | 0.10 | 10 | brms limit 10, gradient against its largest entry |
| `i319_pois_re2` | brms | 302 | 599 | 1.21 | 1.54 |  |  | 0.08 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `i320_gp_expquad` | brms | 34 | 36 | 36,641 | 36,641 | 34,870 | 34,870 | 0.06 | 0.06 | 10 | ill-conditioned, so the 1e-9 limit is 1.5e-6 for the gradient. brms limit 10 |
| `i320_gp_matern32` | brms | 34 | 36 | 1.53 | 0.98 | 0.66 | 0.66 | 0.06 | 0.06 | 10 | brms limit 10 |
| `i320_mi_nhanes` | brms | 27 | 30 | 0.68 | 1.07 | 0.06 | 0.06 | 0.06 | 0.06 | 10 | brms limit 10 |
| `i320_pois_trunc_both` | brms | 4 | 6 | 4.94 | 4.94 | 2.61 | 0.63 | 0.12 | 0.12 | gradient 10 from the true value | brms limit 16, gradient against its largest entry |
| `i320_pois_trunc_ub` | brms | 4 | 6 | 4.94 | 4.94 | 2.61 | 0.63 | 0.12 | 0.12 | gradient 10 from the true value | brms limit 16, gradient against its largest entry |
| `i320_sratio_cs` | brms | 8 | 13 | 8.10 | 8.10 | 3.32 | 3.32 | 0.08 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `i320_sratio_plain` | brms | 6 | 11 | 4.73 | 4.73 | 2.43 | 2.43 | 0.07 | 0.07 | 10 | brms limit 10, gradient against its largest entry |
| `s2_ar_cov` | brms | 4 | 70 | 0.61 | 0.61 | 0.76 | 0.67 | 0.07 | 0.06 | 10 | brms limit 10, gradient against its largest entry |
| `s2_beta_binomial` | brms | 3 | 5 | 4.19 | 4.19 | 1.19 | 0.84 | 0.39 | 0.30 | 10 | brms limit 10 |
| `s2_car` | brms | 10 | 12 | 0.66 | 0.85 | 0.35 | 0.29 | 0.10 | 0.06 | 10 | brms limit 10, gradient against its largest entry |
| `s2_car_esicar` | brms | 8 | 15 | 0.71 | 0.38 | 0.31 | 0.33 | 0.07 | 0.04 | 10 | brms limit 10, gradient against its largest entry |
| `s2_car_icar` | brms | 9 | 16 | 1.15 | 1.15 | 14.8 | 14.8 | 0.12 | 0.09 | 10 | brms limit 10, gradient against its largest entry |
| `s2_categorical_re` | brms | 16 | 29 | 1.29 | 1.63 | 0.61 | 0.64 | 0.17 | 0.15 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cens_interval` | brms | 3 | 5 | 2.04 | 1.04 | 5.0e10 | 5.0e10 | 0.20 | 0.12 | 10 | brms limit 10, gradient against its largest entry |
| `s2_com_poisson` | brms | 3 | 5 | 1.38 | 1.38 | 1.4e15 | 1.4e15 | 0.39 | 0.39 | 10 | brms limit 10 |
| `s2_cosy` | brms | 4 | 70 | 0.87 | 0.60 | 0.79 | 0.77 | 0.07 | 0.07 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cox` | brms | 6 | 9 | 0.94 | 0.26 | 0.50 | 0.17 | 0.57 | 0.53 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cox_cens` | brms | 6 | 9 | 0.35 | 0.35 | 0.86 | 0.63 | 0.57 | 0.53 | 10 | brms limit 10 |
| `s2_cumulative_cauchit` | brms | 4 | 9 | 1.42 | 1.06 | 0.71 | 0.44 | 0.08 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `s2_cumulative_cloglog` | brms | 4 | 9 | 11.8 | 11.8 | 173 | 173 | 0.08 | 0.08 | 10 | brms limit 10 |
| `s2_cumulative_probit` | brms | 4 | 9 | 0.83 | 0.83 | 5.3e9 | 5.3e9 | 0.08 | 0.08 | 10 | brms limit 10 |
| `s2_custom_vint` | brms | 3 | 5 | 3.52 | 3.52 | 1.08 | 1.06 | 0.39 | 0.30 | 10 | brms limit 10 |
| `s2_custom_vreal` | brms | 3 | 5 | 1.18 | 0.85 | 0.55 | 0.39 | 0.21 | 0.13 | 10 | brms limit 10, gradient against its largest entry |
| `s2_dirichlet` | brms | 5 | 8 | 0.13 | 0.65 | 0.31 | 0.29 | 0.25 | 0.19 | 10 | brms limit 10 |
| `s2_discrete_weibull` | brms | 3 | 5 | 0.70 | 0.70 | 0.80 | 0.64 | 0.39 | 0.30 | 10 | brms limit 10, gradient against its largest entry |
| `s2_dist_sigma_re` | brms | 9 | 17 | 0.33 | 0.33 | 0.47 | 0.36 | 0.11 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `s2_fcor` | brms | 3 | 5 | 0.78 | 0.78 | 0.61 | 1.57 | 0.21 | 0.13 | 10 | brms limit 10, gradient against its largest entry |
| `s2_frechet` | brms | 3 | 5 | 0.92 | 0.92 | 2.00 | 1.91 | 0.20 | 0.12 | 10 | brms limit 10 |
| `s2_gev` | brms | 4 | 6 | 0.29 | 0.29 | 1.32 | 1.43 | 0.19 | 0.11 | 10 | brms limit 10, gradient against its largest entry |
| `s2_gp_approx` | brms | 9 | 11 | 0.30 | 0.63 | 0.33 | 0.29 | 0.08 | 0.08 | 10 | brms limit 10 |
| `s2_gp_by_approx` | brms | 16 | 18 | 0.43 | 0.45 | 0.15 | 0.14 | 0.11 | 0.11 | 10 | brms limit 10 |
| `s2_gp_by_gr` | brms | 52 | 54 | 1.7e6 | 1.7e6 | 1.9e7 | 1.9e7 | 0.04 | 0.04 | 10 | ill-conditioned, so the 1e-9 limit is 1.5e-6 for the gradient. brms limit 10 |
| `s2_gr_by` | brms | 10 | 17 | 0.39 | 0.37 | 0.28 | 0.25 | 0.14 | 0.12 | 10 | brms limit 10, gradient against its largest entry |
| `s2_gr_student` | brms | 15 | 27 | 0.83 | 1.17 | 0.31 | 0.36 | 0.20 | 0.18 | 10 | brms limit 10 |
| `s2_hurdle_cumulative` | brms | 4 | 8 | 1.44 | 1.60 | 0.96 | 0.63 | 0.06 | 0.06 | 10 | brms limit 10, gradient against its largest entry |
| `s2_hurdle_negbin` | brms | 4 | 6 | 1.10 | 1.10 | 1.70 | 1.66 | 0.25 | 0.18 | 10 | brms limit 10 |
| `s2_index_mi` | brms | 9 | 12 | 0.30 | 0.35 | 0.36 | 0.37 | 0.08 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `s2_invgaussian` | brms |  |  |  |  |  |  |  |  |  | CmdStan rejects every point, and stanli must too |
| `s2_logistic_normal` | brms | 7 | 18 | 1.57 | 1.90 | 0.65 | 0.69 | 0.10 | 0.07 | 10 | brms limit 10 |
| `s2_me2` | brms | 89 | 259 | 0.45 | 0.71 | 0.05 | 0.06 | 0.10 | 0.10 | 10 | brms limit 10 |
| `s2_me2_nomecor` | brms | 88 | 170 | 0.61 | 0.61 | 0.06 | 0.05 | 0.06 | 0.06 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mi_lognormal` | brms | 9 | 12 | 0.36 | 0.39 | 0.59 | 0.58 | 2.53 | 0.29 | outputs 10 from the true value | brms limit 64 |
| `s2_mi_trunc_lb` | brms | 9 | 12 | 0.75 | 0.75 | 1.2e8 | 1.2e8 | 2.53 | 0.29 | outputs 10 from the true value | brms limit 64 |
| `s2_mixture_theta` | brms | 8 | 14 | 0.54 | 0.63 | 0.39 | 0.32 | 0.14 | 0.14 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mm` | brms | 9 | 16 | 0.51 | 0.51 | 0.50 | 0.48 | 0.10 | 0.07 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mm_weights` | brms | 9 | 16 | 0.53 | 0.53 | 0.36 | 0.32 | 0.10 | 0.07 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mmc` | brms | 16 | 46 | 0.22 | 0.89 | 0.23 | 0.27 | 0.13 | 0.12 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mo_simo_prior` | brms | 5 | 8 | 0.59 | 0.77 | 1.00 | 0.88 | 0.27 | 0.27 | 10 | brms limit 10 |
| `s2_multinomial` | brms | 4 | 7 | 1.03 | 1.03 | 0.92 | 0.46 | 0.23 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mv_shared_re` | brms | 19 | 50 | 0.51 | 1.42 | 0.40 | 0.38 | 0.09 | 0.08 | 10 | brms limit 10, gradient against its largest entry |
| `s2_mv_subset` | brms | 6 | 9 | 0.37 | 0.49 | 0.74 | 0.74 | 0.41 | 0.41 | 10 | brms limit 10 |
| `s2_nl_noloop` | brms | 5 | 6 | 0.81 | 0.81 | 1.07 | 1.07 | 0.17 | 0.17 | 10 | brms limit 10 |
| `s2_nlf` | brms | 3 | 4 | 1.84 | 1.84 | 0.62 | 0.62 | 0.20 | 0.20 | 10 | CmdStan rejects point 1 and 2, and stanli must too. brms limit 10 |
| `s2_rate` | brms | 2 | 4 | 1.75 | 0.62 | 2.71 | 2.22 | 0.27 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `s2_s_by` | brms | 52 | 94 | 1.73 | 1.73 | 0.10 | 0.09 | 0.07 | 0.07 | 10 | brms limit 10 |
| `s2_s_cc` | brms | 11 | 21 | 0.30 | 0.30 | 0.21 | 0.21 | 0.11 | 0.11 | 10 | brms limit 10 |
| `s2_sar` | brms | 4 | 6 | 1.45 | 1.45 | 2.90 | 2.80 | 7.5e13 | 7.5e13 | 10 | brms limit 10, gradient against its largest entry |
| `s2_sar_error` | brms | 4 | 6 | 1.81 | 1.81 | 2.22 | 2.64 | 7.5e13 | 7.5e13 | 10 | brms limit 10, gradient against its largest entry |
| `s2_shifted_lognormal` | brms | 4 | 6 | 0.86 | 0.86 | 0.82 | 0.69 | 0.27 | 0.20 | 10 | brms limit 10 |
| `s2_t2_by` | brms | 56 | 100 | 0.51 | 0.18 | 0.09 | 0.09 | 0.08 | 0.08 | 10 | brms limit 10 |
| `s2_threading` | brms | 4 | 6 | 0.41 | 0.41 | 1.84 | 1.95 | 0.31 | 0.23 | 10 | brms limit 10 |
| `s2_unstr` | brms | 31 | 161 | 0.99 | 0.99 | 0.27 | 0.25 | 0.17 | 0.15 | 10 | brms limit 10, gradient against its largest entry |
| `s2_weights_trunc` | brms | 3 | 5 | 1.14 | 1.48 | 5.0e8 | 5.0e8 | 0.20 | 0.12 | 10 | brms limit 10, gradient against its largest entry |
| `s2_wiener` | brms | 5 | 7 |  |  |  |  |  |  | 10 | brms limit 10, gradient against its largest entry |
| `s2_zi_asymlaplace` | brms | 5 | 7 | 1.62 | 1.62 | 1.02 | 1.03 | 0.23 | 0.17 | 10 | brms limit 10 |
| `s2_zi_beta` | brms | 4 | 6 | 0.94 | 0.94 | 0.78 | 0.39 | 0.35 | 0.28 | 10 | brms limit 10, gradient against its largest entry |
| `s2_zoi_beta` | brms | 5 | 7 | 1.01 | 1.68 | 0.60 | 0.20 | 0.34 | 0.29 | 10 | brms limit 10, gradient against its largest entry |
| `sw_acat` | brms | 4 | 9 | 15.2 | 15.2 | 9.35 | 9.35 | 0.05 | 0.05 | 10 | brms limit 10, gradient against its largest entry |
| `sw_acat_cs` | brms | 7 | 12 | 21.9 | 21.9 | 6.54 | 6.54 | 0.03 | 0.03 | 10 | brms limit 10, gradient against its largest entry |
| `sw_ar` | brms | 4 | 6 | 0.93 | 0.93 | 0.68 | 0.74 | 0.37 | 0.19 | 10 | brms limit 10, gradient against its largest entry |
| `sw_arma` | brms | 5 | 7 | 0.48 | 0.48 | 0.64 | 0.59 | 0.30 | 0.15 | 10 | brms limit 10, gradient against its largest entry |
| `sw_asymlaplace` | brms | 4 | 6 | 0.37 | 0.71 | 0.96 | 1.02 | 0.32 | 0.17 | 10 | brms limit 10 |
| `sw_bernoulli` | brms | 3 | 5 | 0.59 | 0.59 | 1.74 | 1.12 | 0.27 | 0.15 | 10 | brms limit 10, gradient against its largest entry |
| `sw_beta` | brms | 3 | 5 | 2.20 | 2.20 | 1.27 | 0.38 | 0.53 | 0.34 | 10 | brms limit 10 |
| `sw_binomial` | brms | 2 | 4 | 4.55 | 5.89 | 0.60 | 0.48 | 0.49 | 0.24 | 10 | brms limit 10, gradient against its largest entry |
| `sw_categorical` | brms | 4 | 7 | 3.64 | 0.97 | 1.55 | 1.08 | 0.37 | 0.19 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cens` | brms | 3 | 5 | 0.57 | 0.57 | 1.8e9 | 1.8e9 | 0.35 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cratio` | brms | 4 | 9 | 16.7 | 16.7 | 8.09 | 8.09 | 0.05 | 0.05 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cratio_cs` | brms | 7 | 12 | 3.18 | 3.18 | 4.86 | 4.86 | 0.03 | 0.03 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cumulative` | brms | 5 | 10 | 21.3 | 21.3 | 7.79 | 7.79 | 0.12 | 0.12 | 10 | brms limit 10, gradient against its largest entry |
| `sw_cumulative_cs` | brms | 7 | 12 | 19.2 | 19.2 | 7.95 | 7.95 | 0.05 | 0.05 | 10 | brms limit 10, gradient against its largest entry |
| `sw_dist_sigma` | brms | 4 | 7 | 0.65 | 0.65 | 0.78 | 0.64 | 0.26 | 0.10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_exgaussian` | brms | 4 | 6 | 0.57 | 0.57 | 0.48 | 0.65 | 0.30 | 0.15 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gamma` | brms | 3 | 5 | 0.83 | 0.92 | 0.89 | 0.81 | 0.51 | 0.32 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gaussian` | brms | 4 | 6 | 0.71 | 0.71 | 0.63 | 0.59 | 0.19 | 0.10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_gp` | brms | 44 | 46 | 2.0e5 | 2.0e5 | 1.6e6 | 1.6e6 | 0.02 | 0.02 | 10 | ill-conditioned, so the 1e-9 limit is 1.5e-6 for the gradient. brms limit 10 |
| `sw_hurdle_gamma` | brms | 4 | 6 | 0.91 | 1.25 | 0.96 | 0.39 | 0.44 | 0.29 | 10 | brms limit 10, gradient against its largest entry |
| `sw_hurdle_lognormal` | brms | 4 | 6 | 0.80 | 0.46 | 1.02 | 0.39 | 0.32 | 0.17 | 10 | brms limit 10, gradient against its largest entry |
| `sw_hurdle_pois` | brms | 3 | 5 | 1.95 | 0.16 | 0.35 | 0.17 | 0.39 | 0.20 | 10 | brms limit 10, gradient against its largest entry |
| `sw_lognormal` | brms | 3 | 5 | 0.51 | 0.51 | 0.79 | 0.85 | 0.35 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `sw_ma` | brms | 4 | 6 | 1.08 | 1.08 | 0.73 | 0.78 | 0.37 | 0.19 | 10 | brms limit 10, gradient against its largest entry |
| `sw_me` | brms | 45 | 87 | 0.67 | 0.94 | 0.08 | 0.08 | 0.17 | 0.17 | 10 | brms limit 10 |
| `sw_mi` | brms | 9 | 12 | 0.68 | 0.39 | 0.72 | 0.72 | 0.20 | 0.17 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mixture` | brms | 7 | 15 | 0.93 | 1.42 | 0.95 | 0.70 | 0.24 | 0.19 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mono` | brms | 5 | 8 | 0.51 | 0.51 | 0.63 | 0.70 | 0.21 | 0.21 | 10 | brms limit 10 |
| `sw_mv_norescor` | brms | 6 | 9 | 0.43 | 0.72 | 0.64 | 0.48 | 0.30 | 0.14 | 10 | brms limit 10, gradient against its largest entry |
| `sw_mv_rescor` | brms | 7 | 18 | 0.57 | 0.44 | 0.57 | 0.44 | 0.16 | 0.09 | 10 | brms limit 10, gradient against its largest entry |
| `sw_negbinomial` | brms | 4 | 6 | 2.61 | 2.61 | 0.48 | 0.57 | 0.22 | 0.14 | 10 | brms limit 10, gradient against its largest entry |
| `sw_nonlinear` | brms | 3 | 4 | 0.59 | 0.59 | 0.76 | 0.76 | 0.12 | 0.12 | 10 | brms limit 10 |
| `sw_poisson` | brms | 3 | 5 | 0.21 | 0.21 | 0.21 | 0.23 | 0.29 | 0.18 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_bern` | brms | 8 | 15 | 0.20 | 0.20 | 0.45 | 0.30 | 0.21 | 0.14 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_gauss` | brms | 9 | 16 | 0.49 | 0.49 | 0.64 | 0.66 | 0.17 | 0.10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_negbin` | brms | 9 | 16 | 1.70 | 1.70 | 0.27 | 0.26 | 0.15 | 0.09 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_pois` | brms | 8 | 15 | 0.42 | 0.42 | 0.13 | 0.10 | 0.17 | 0.10 | 10 | brms limit 10, gradient against its largest entry |
| `sw_re_slope` | brms | 16 | 46 | 0.52 | 0.85 | 0.49 | 0.46 | 0.16 | 0.13 | 10 | brms limit 10, gradient against its largest entry |
| `sw_se` | brms | 2 | 5 | 0.43 | 0.43 | 1.22 | 1.10 | 0.41 | 0.16 | 10 | brms limit 10 |
| `sw_skewnormal` | brms | 4 | 6 | 1.29 | 1.29 | 1.91 | 1.55 | 0.30 | 0.13 | 10 | brms limit 10, gradient against its largest entry |
| `sw_spline_s` | brms | 12 | 22 | 0.92 | 0.92 | 0.11 | 0.13 | 0.12 | 0.12 | 10 | brms limit 10 |
| `sw_spline_t2` | brms | 29 | 52 | 0.61 | 0.61 | 0.13 | 0.14 | 0.07 | 0.07 | 10 | brms limit 10 |
| `sw_sratio` | brms | 4 | 9 | 16.7 | 16.7 | 8.09 | 8.09 | 0.05 | 0.05 | 10 | brms limit 10, gradient against its largest entry |
| `sw_student` | brms | 5 | 7 | 0.33 | 0.33 | 0.31 | 0.44 | 0.33 | 0.26 | 10 | brms limit 10, gradient against its largest entry |
| `sw_trunc` | brms | 3 | 5 | 0.31 | 0.31 | 1.1e9 | 1.1e9 | 0.35 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `sw_vonmises` | brms | 3 | 5 | 0.91 | 0.91 | 0.54 | 0.44 | 0.38 | 0.19 | 10 | brms limit 10, gradient against its largest entry |
| `sw_weibull` | brms | 3 | 5 | 1.11 | 1.11 | 0.38 | 0.40 | 0.51 | 0.32 | 10 | brms limit 10, gradient against its largest entry |
| `sw_weights` | brms | 3 | 5 | 1.62 | 0.50 | 2.17 | 1.38 | 0.35 | 0.16 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_binomial` | brms | 3 | 5 | 4.43 | 5.10 | 0.56 | 0.37 | 0.42 | 0.23 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_negbin` | brms | 4 | 6 | 0.83 | 0.50 | 0.92 | 0.68 | 0.36 | 0.21 | 10 | brms limit 10, gradient against its largest entry |
| `sw_zi_poisson` | brms | 3 | 5 | 1.00 | 0.52 | 0.32 | 0.34 | 0.39 | 0.20 | 10 | brms limit 10, gradient against its largest entry |
| `ch09_m5_8s` | rethinking | 4 | 4 | 0.57 | 0.57 | 0.52 | 0.34 | 0.07 | 0.07 | 10 |  |
| `ch09_m5_8s2` | rethinking | 4 | 4 | 0.70 | 0.70 | 1.02 | 0.66 | 0.09 | 0.09 | 10 |  |
| `ch09_m9_1` | rethinking | 5 | 5 | 0.22 | 0.22 | 0.43 | 0.19 | 0.05 | 0.05 | 10 |  |
| `ch09_m9_1_chains4` | rethinking | 5 | 5 | 0.22 | 0.22 | 0.43 | 0.19 | 0.05 | 0.05 | 10 |  |
| `ch09_m9_2` | rethinking | 2 | 2 | 0.63 | 0.63 | 0.71 | 0.71 | 0.11 | 0.11 | 10 |  |
| `ch09_m9_3` | rethinking | 2 | 2 | 0.53 | 0.53 | 6.29 | 6.29 | 0.11 | 0.11 | 10 |  |
| `ch09_m9_4` | rethinking | 3 | 3 | 0.90 | 0.90 | 1.62 | 1.62 | 0.05 | 0.05 | 10 |  |
| `ch09_m9_5` | rethinking | 3 | 3 | 0.66 | 0.66 | 1.67 | 1.67 | 0.05 | 0.05 | 10 |  |
| `ch09_mp` | rethinking | 2 | 2 | 0.36 | 0.36 | 0.09 | 0.09 | <0.01 | <0.01 | 10 |  |
| `ch11_m11_10` | rethinking | 4 | 24 | 0.94 | 0.94 | 0.33 | 0.42 | 0.23 | 0.23 | 10 |  |
| `ch11_m11_11` | rethinking | 5 | 25 | 1.86 | 1.86 | 0.79 | 0.60 | 0.58 | 0.58 | 10 |  |
| `ch11_m11_4` | rethinking | 11 | 1019 | 12.9 | 12.9 | 0.77 | 0.20 | 0.26 | 0.26 | 10 |  |
| `ch11_m11_5` | rethinking | 11 | 1019 | 15.1 | 15.4 | 1.73 | 0.15 | 0.27 | 0.27 | gradient 10 from the true value |  |
| `ch11_m11_6` | rethinking | 11 | 67 | 0.91 | 0.91 | 0.21 | 0.20 | 0.52 | 0.52 | 10 |  |
| `ch11_m11_7` | rethinking | 2 | 2 | 0.70 | 0.70 | 0.35 | 0.29 | <0.01 | <0.01 | 10 |  |
| `ch11_m11_8` | rethinking | 8 | 8 | 0.68 | 0.68 | 0.21 | 0.37 | <0.01 | <0.01 | 10 |  |
| `ch11_m11_9` | rethinking | 1 | 12 | 9.65 | 9.65 | 0.62 | 0.62 | 0.33 | 0.33 | 10 |  |
| `ch11_m_pois` | rethinking | 2 | 2 | 4.30 | 4.30 | 0.31 | 0.31 | <0.01 | <0.01 | 10 |  |
| `ch12_m12_1` | rethinking | 3 | 4 | 1.30 | 1.30 | 4.20 | 3.93 | 0.06 | 0.06 | 10 |  |
| `ch12_m12_2` | rethinking | 6 | 26 | 4.31 | 0.48 | 4.01 | 0.31 | 5.35 | 0.47 | gradient 10 from the true value, outputs 10 from the true value |  |
| `ch12_m12_3` | rethinking | 2 | 2 | 24.9 | 0.50 | 24.4 | 0.99 | <0.01 | <0.01 | lp 10 from the true value, gradient 10 from the true value |  |
| `ch12_m12_3_alt` | rethinking | 2 | 2 | 14.0 | 0.50 | 81.9 | 0.99 | <0.01 | <0.01 | lp 10 from the true value, gradient 10 from the true value |  |
| `ch12_m12_4` | rethinking | 6 | 6 | 296 | 0.38 | 71.0 | 0.65 | 0.07 | 0.07 | lp 10 from the true value, gradient 10 from the true value |  |
| `ch12_m12_5` | rethinking | 11 | 11 | 144 | 145 | 33.1 | 0.24 | 0.04 | 0.04 | gradient 10 from the true value |  |
| `ch12_m12_6` | rethinking | 16 | 17 | 148 | 148 | 24.5 | 7.71 | 0.17 | 0.17 | gradient 10 from the true value |  |
| `ch12_m12_7` | rethinking | 10 | 10 | 169 | 169 | 33.9 | 0.35 | 0.10 | 0.10 | gradient 10 from the true value |  |
| `ch13_m13_1` | rethinking | 48 | 144 | 1.43 | 1.10 | 0.15 | 0.15 | 0.25 | 0.25 | 10 |  |
| `ch13_m13_2` | rethinking | 50 | 146 | 1.20 | 0.87 | 0.09 | 0.09 | 0.25 | 0.25 | 10 |  |
| `ch13_m13_3` | rethinking | 62 | 62 | 0.71 | 0.71 | 0.08 | 0.07 | 0.01 | 0.01 | 10 |  |
| `ch13_m13_4` | rethinking | 20 | 1028 | 15.8 | 16.2 | 0.36 | 0.13 | 0.28 | 0.28 | 10 |  |
| `ch13_m13_4b` | rethinking | 20 | 20 | 15.8 | 16.2 | 0.36 | 0.13 | 0.01 | 0.01 | 10 |  |
| `ch13_m13_4nc` | rethinking | 20 | 33 | 9.02 | 9.02 | 0.45 | 0.11 | 0.07 | 0.07 | 10 |  |
| `ch13_m13_5` | rethinking | 13 | 1021 | 21.4 | 21.8 | 0.85 | 0.19 | 0.25 | 0.25 | 10 |  |
| `ch13_m13_6` | rethinking | 21 | 1029 | 15.7 | 16.1 | 0.35 | 0.12 | 0.28 | 0.28 | 10 |  |
| `ch13_m13_7` | rethinking | 2 | 2 | 5.07 | 5.07 | 0.25 | 0.25 | <0.01 | <0.01 | 10 |  |
| `ch13_m13_7nc` | rethinking | 2 | 3 | 0.52 | 0.52 | 0.35 | 0.35 | 0.10 | 0.10 | 10 |  |
| `ch14_m14_1` | rethinking | 46 | 49 | 0.50 | 0.50 | 0.04 | 0.04 | 0.02 | 0.02 | 10 |  |
| `ch14_m14_10` | rethinking | 4 | 4 | 3.57 | 3.57 | 5.96 | 4.46 | 0.07 | 0.07 | gradient 28 |  |
| `ch14_m14_11` | rethinking | 5 | 5 | 6.71 | 6.71 | 6.32 | 6.41 | 0.10 | 0.10 | gradient 32 |  |
| `ch14_m14_2` | rethinking | 76 | 96 | 14.0 | 13.3 | 0.15 | 0.11 | 0.03 | 0.03 | 10 |  |
| `ch14_m14_3` | rethinking | 76 | 1188 | 8.16 | 7.82 | 0.16 | 0.12 | 0.24 | 0.24 | 10 |  |
| `ch14_m14_4` | rethinking | 3 | 3 | 1.06 | 1.06 | 1.45 | 1.00 | 0.05 | 0.05 | 10 |  |
| `ch14_m14_4x` | rethinking | 3 | 3 | 1.06 | 1.06 | 1.45 | 1.00 | 0.05 | 0.05 | 10 |  |
| `ch14_m14_5` | rethinking | 4 | 4 | 1.41 | 1.41 | 1.20 | 0.46 | 0.07 | 0.07 | 10 |  |
| `ch14_m14_6` | rethinking | 7 | 10 | 3.08 | 2.75 | 2.45 | 1.86 | 0.05 | 0.05 | 10 |  |
| `ch14_m14_6x` | rethinking | 7 | 10 | 3.08 | 2.75 | 2.45 | 1.86 | 0.05 | 0.05 | 10 |  |
| `ch14_m14_7` | rethinking | 656 | 1266 | 3.79 | 3.79 |  |  | 0.16 | 0.16 | gradient 13 |  |
| `ch14_m14_8` | rethinking | 15 | 15 | 4.08 | 3.08 | 0.27 | 0.29 | 0.08 | 0.08 | 10 |  |
| `ch14_m14_8nc` | rethinking | 15 | 225 | 2.24 | 2.24 | 0.39 | 0.32 | 0.06 | 0.06 | 10 |  |
| `ch14_m14_9` | rethinking | 4 | 4 | 0.37 | 0.37 | 0.71 | 0.21 | 0.07 | 0.07 | 10 |  |
| `ch15_m15_1` | rethinking | 54 | 54 | 0.78 | 0.78 | 0.10 | 0.10 | <0.01 | <0.01 | 10 |  |
| `ch15_m15_2` | rethinking | 104 | 104 | 0.56 | 0.56 | 0.06 | 0.06 | <0.01 | <0.01 | 10 |  |
| `ch15_m15_3` | rethinking | 2 | 2 | 40.4 | 40.4 | 14.7 | 0.25 | <0.01 | <0.01 | gradient 10 from the true value |  |
| `ch15_m15_4` | rethinking | 2 | 2 | 31.4 | 31.4 | 14.3 | 0.25 | <0.01 | <0.01 | gradient 10 from the true value |  |
| `ch15_m15_5` | rethinking | 18 | 18 | 0.90 | 0.90 | 0.17 | 0.14 | 0.03 | 0.03 | 10 |  |
| `ch15_m15_6` | rethinking | 6 | 6 | 0.39 | 0.39 | 0.83 | 0.80 | 0.09 | 0.09 | 10 |  |
| `ch15_m15_7` | rethinking | 21 | 24 | 0.40 | 0.73 | 0.36 | 0.33 | 0.02 | 0.02 | 10 |  |
| `ch15_m15_8` | rethinking | 3 | 3 | 5.86 | 1.27 | 0.93 | 0.14 | 0.08 | 0.08 | 10 |  |
| `ch15_m15_9` | rethinking | 3 | 303 | 5.86 | 1.27 | 2.13 | 0.14 | 0.33 | 0.33 | gradient 10 from the true value |  |
| `ch16_m16_1` | rethinking | 3 | 3 | 0.40 | 1.06 | 2.96 | 2.33 | 0.21 | 0.21 | 10 |  |
| `ch16_m16_4` | rethinking | 3 | 3 | 1.19 | 1.19 | 1.32 | 1.25 | 0.17 | 0.17 | 10 |  |
| `extra_hurdle_poisson` | rethinking | 2 | 2 | 0.70 | 0.37 | 0.50 | 0.68 | 0.26 | 0.26 | 10 |  |
| `cm_betadiscrete` | cogmod | 4 | 8 | 2.04 | 2.37 | 35.9 | 35.2 | 0.12 | 0.12 | 10 |  |
| `cm_betagate` | cogmod | 5 | 10 | 1.37 | 1.08 | 1.41 | 0.71 | 0.14 | 0.14 | 10 |  |
| `cm_bisa` | cogmod | 5 | 10 | 3.27 | 3.27 | 7.19 | 5.66 | 0.11 | 0.11 | gradient 10 from the true value |  |
| `cm_choco` | cogmod | 9 | 18 | 0.64 | 0.64 | 1.98 | 0.50 | 0.04 | 0.04 | gradient 10 from the true value |  |
| `cm_ddm` | cogmod | 9 | 18 |  |  |  |  |  |  | gradient 19 |  |
| `cm_exgaussian` | cogmod | 4 | 8 | 1.46 | 1.46 | 1.20 | 0.87 | 0.13 | 0.13 | 10 |  |
| `cm_exwald` | cogmod | 6 | 12 | 2.54 | 2.54 | 2.07 | 0.14 | 0.08 | 0.08 | gradient 10 from the true value |  |
| `cm_gamma` | cogmod | 5 | 10 | 2.02 | 2.02 | 2.13 | 0.89 | 0.07 | 0.07 | gradient 10 from the true value |  |
| `cm_geg` | cogmod | 5 | 10 | 1.85 | 1.85 | 1.37 | 0.76 | 0.17 | 0.17 | 10 |  |
| `cm_invgamma` | cogmod | 5 | 10 | 0.93 | 0.93 | 2.29 | 2.05 | 0.07 | 0.07 | 10 |  |
| `cm_invgaussian` | cogmod | 7 | 14 | 2.17 | 2.50 | 1.31 | 0.44 | 0.06 | 0.06 | gradient 10 from the true value |  |
| `cm_invweibull` | cogmod | 5 | 10 | 0.29 | 0.62 | 4.89 | 4.62 | 0.07 | 0.07 | 10 |  |
| `cm_lba1` | cogmod | 7 | 14 | 3.62 | 2.95 | 7.5e5 | 7.5e5 | 0.05 | 0.05 | gradient 10 from the true value |  |
| `cm_lba2` | cogmod | 9 | 18 | 2.28 | 2.59 | 1.45 | 0.07 | 0.07 | 0.07 | gradient 10 from the true value |  |
| `cm_lnr` | cogmod | 8 | 16 | 0.65 | 1.00 | 1.51 | 0.19 | 0.08 | 0.08 | gradient 10 from the true value |  |
| `cm_lnr_bench` | cogmod | 9 | 16 | 10.6 | 10.6 | 3.68 | 3.36 | 1.37 | 1.37 | 10 |  |
| `cm_loggamma` | cogmod | 6 | 12 | 44.8 | 45.4 | 18.5 | 18.4 | 0.07 | 0.07 | 10 |  |
| `cm_lognormal` | cogmod | 6 | 12 | 1.84 | 2.24 | 0.85 | 0.57 | 0.07 | 0.07 | 10 |  |
| `cm_logstudent` | cogmod | 6 | 12 | 1.61 | 1.94 | 3.22 | 1.37 | 0.07 | 0.07 | gradient 10 from the true value |  |
| `cm_logweibull` | cogmod | 5 | 10 | 2.20 | 1.87 | 0.81 | 0.56 | 0.08 | 0.08 | 10 |  |
| `cm_rdm` | cogmod | 7 | 14 | 2.45 | 2.78 | 1.11 | 0.16 | 0.09 | 0.09 | gradient 10 from the true value |  |
| `cm_weibull` | cogmod | 5 | 10 | 2.57 | 2.57 | 2.31 | 0.99 | 0.07 | 0.07 | gradient 10 from the true value |  |
| `ctsem_ctsm` | ctsem | 181 | 648 | 2.79 | 0.68 | 0.02 | 0.02 | 0.09 | 0.09 | 10 |  |
| `ctsem_ctsm_400` | ctsem | 1621 | 4968 | 6.18 | 0.69 |  |  | 0.02 | 0.02 | 10 |  |
| `ctsem_ctsm_post4_100` | ctsem | 580 | 4929 | 3.52 | 0.52 |  |  | 0.30 | 0.30 | gradient 22 |  |
| `examplemodels_arch1` | examplemodels | 3 | 3 | 0.72 | 0.72 | 0.34 | 0.34 | 0.14 | 0.14 | 10 |  |
| `examplemodels_cjs_cov_raneff` | examplemodels | 23 | 16574 | 3.20 | 0.42 | 2.27 | 1.77 | 0.10 | 0.10 | gradient 10 from the true value |  |
| `examplemodels_cjs_mnl_ran` | examplemodels | 21 | 0 | 0.29 | 0.94 | 0.47 | 0.24 |  |  | 10 |  |
| `examplemodels_covid_sens_spec` | examplemodels | 23 | 41 | 1.31 | 0.98 | 0.05 | 0.05 | 0.20 | 0.18 | 10 |  |
| `examplemodels_dlm_fx_equicorr` | examplemodels | 7 | 25 |  |  |  |  |  |  | 10 |  |
| `examplemodels_dlm_fx_factor` | examplemodels | 7 | 10 |  |  |  |  |  |  | 10 |  |
| `examplemodels_dlm_nile` | examplemodels | 2 | 2 |  |  |  |  |  |  | 10 |  |
| `examplemodels_gp_fit_multi_output` | examplemodels | 311 | 326 | 1.8e7 | 1.8e7 |  |  | 0.01 | 0.01 | lp 5.2e07, gradient 4.2e08 | ill-conditioned, so the 1e-9 limit is 1.5e-6 for the gradient and 5e-8 for lp |
| `examplemodels_hmm_marginal` | examplemodels | 10 | 0 |  |  |  |  |  |  | 10 |  |
| `examplemodels_irt_1pl_hier` | examplemodels | 33 | 33 | 0.89 | 0.89 | 0.08 | 0.07 | 0.02 | 0.02 | 10 |  |
| `examplemodels_irt_multilevel` | examplemodels | 55 | 55 | 6.15 | 6.48 | 0.09 | 0.07 | 0.04 | 0.04 | 10 |  |
| `examplemodels_kidney` | examplemodels | 46 | 51 | 1.07 | 1.07 | 0.08 | 0.08 | 0.01 | 0.01 | 10 |  |
| `examplemodels_mr_ss` | examplemodels | 2 | 19602 | 14.7 | 14.4 | 277 | 110 | 0.14 | 0.14 | gradient 233 from the true value |  |
| `examplemodels_ms` | examplemodels | 6 | 72036 | 7.19 | 1.01 | 36.4 | 13.3 | 0.05 | 0.05 | gradient 53 from the true value |  |
| `examplemodels_ode_one_comp_mm` | examplemodels | 4 | 44 |  |  |  |  |  |  | gradient 30, outputs 13 |  |
| `examplemodels_probit_multi_good` | examplemodels | 2038 | 2064 | 87.8 | 61.1 |  |  | 0.17 | 0.18 | lp 75 |  |
| `examplemodels_radon_group` | examplemodels | 92 | 92 | 4.99 | 4.33 | 0.09 | 0.08 | 0.15 | 0.14 | 10 |  |
| `examplemodels_roaches_overdispersion` | examplemodels | 267 | 267 | 3.05 | 3.05 | 0.01 | 0.01 | 0.33 | 0.33 | 10 |  |
| `examplemodels_ssm` | examplemodels | 28 | 55 | 1.92 | 1.83 | 1.13 | 1.13 | 0.24 | 0.24 | 10 |  |
| `examplemodels_stagnant2` | examplemodels | 5 | 5 | 0.26 | 0.60 | 0.52 | 0.52 | 53.2 | 53.2 | 10 |  |
| `examplemodels_stochvol_optimized` | examplemodels | 503 | 1003 | 1.05 | 1.05 |  |  | 0.14 | 0.14 | 10 |  |
| `examplemodels_sum_to_zero_binomial` | examplemodels | 19 | 833 | 1.35 | 1.35 | 0.53 | 0.14 | 0.28 | 0.28 | 10 |  |
| `examplemodels_sur` | examplemodels | 27 | 33 | 0.29 | 0.29 | 0.22 | 0.22 | 0.03 | 0.03 | 10 |  |
| `stanguide_changepoint` | stanguide | 2 | 114 | 3.37 | 3.37 | 138 | 138 | 3.15 | 3.15 | 10 |  |
| `stanguide_cjs_history` | stanguide | 5 | 8 | 0.57 | 0.24 | 1.02 | 1.04 | 0.20 | 0.23 | 10 |  |
| `stanguide_copula` | stanguide | 6 | 21 |  |  |  |  |  |  | 10 |  |
| `stanguide_integrate_1d_normal` | stanguide | 3 | 3 |  |  |  |  |  |  | 10 |  |
| `stanguide_map_rect_logistic` | stanguide | 2 | 2 |  |  |  |  |  |  | 10 |  |
| `stanguide_ode_adjoint` | stanguide | 5 | 5 |  |  |  |  |  |  | gradient 2.0e10 | known mismatch in grad[1], grad[2], grad[5], which have their own relative limit |
| `oncobayes2_blrm_exnex_combo2` | oncobayes2 | 52 | 143 | 0.71 | 0.37 | 0.09 | 0.09 | 0.07 | 0.07 | 10 |  |
| `oncobayes2_blrm_exnex_combo3` | oncobayes2 | 99 | 271 | 1.37 | 1.07 | 0.53 | 0.49 | 0.31 | 0.32 | outputs 14 |  |
| `oncobayes2_blrm_exnex_sa` | oncobayes2 | 9 | 23 |  |  |  |  |  |  | 10 |  |
| `rstanarm_glm_gaussian` | rstanarm | 4 | 9 |  |  |  |  |  |  | 10 |  |
| `rstanarm_glmer_bernoulli` | rstanarm | 71 | 142 |  |  |  |  |  |  | 10 |  |
| `rstanarm_glmer_nb_count` | rstanarm | 25 | 51 |  |  |  |  |  |  | 10 |  |
| `rstanarm_polr` | rstanarm | 9 | 113 |  |  |  |  |  |  | 10 | CmdStan rejects point 2, and stanli must too |
| `multinma_ndmm_mspline` | multinma | 62 | 956 |  |  |  |  |  |  | 10 |  |
| `multinma_smoking_re` | multinma | 59 | 375 |  |  |  |  |  |  | 10 |  |
| `epinow2_estimate_infections` | epinow2 | 24 | 352 | 1.4e6 | 1.4e6 | 1.6e6 | 1.6e6 | 5.4e5 | 5.4e5 | gradient 34,141, outputs 24 |  |
| `epinow2_estimate_secondary` | epinow2 | 4 | 72 | 105 | 4.27 | 38.3 | 10.9 | 18.0 | 18.0 | lp 10 from the true value, gradient 10 from the true value |  |
| `mlumr_survival_mspline_relaxed` | mlumr | 32 | 1052 | 1.03 | 0.53 | 0.24 | 0.18 | 3.34 | 3.57 | outputs 128 |  |
| `mlumr_survival_relaxed` | mlumr | 12 | 925 | 1.52 | 2.23 | 0.69 | 0.69 | 1.71 | 1.70 | outputs 10 from the true value |  |
| `rbioacc_odetk` | rbioacc | 3 | 124 |  |  |  |  |  |  | 10 |  |
| `beeguts_guts_sd` | beeguts | 4 | 540 |  |  |  |  |  |  | 10 |  |
| `hdbayes_lm_npp` | hdbayes | 6 | 6 |  |  |  |  |  |  | 10 |  |
| `hdbayes_pwe_npp` | hdbayes | 8 | 64 |  |  |  |  |  |  | 10 |  |
| `ecoensemble_hierarchical_withdrivers` | ecoensemble | 63 | 612 |  |  |  |  |  |  | outputs 16 |  |
| `ecoensemble_hierarchical_withdrivers_conj` | ecoensemble | 63 | 612 |  |  |  |  |  |  | outputs 16 |  |
| `hbayesdm_dd_hyperbolic` | hbayesdm | 44 | 2266 | 42,015 | 42,014 | 39,048 | 39,048 | 6,116 | 6,116 | 10 |  |
| `hbayesdm_gng_m4` | hbayesdm | 72 | 14548 | 9.66 | 9.66 | 0.18 | 0.16 | 0.12 | 0.11 | 10 |  |
| `hbayesdm_igt_pvl_delta` | hbayesdm | 24 | 448 | 1.81 | 1.81 | 0.32 | 0.31 | 0.26 | 0.29 | 10 |  |
| `hbayesdm_prl_rp` | hbayesdm | 66 | 8149 | 3.02 | 3.02 | 0.06 | 0.06 | 0.34 | 0.34 | 10 |  |
| `bmgarch_bekk_arma` | bmgarch | 27 | 0 |  |  |  |  |  |  | 10 |  |
| `bmgarch_ccc` | bmgarch | 22 | 0 |  |  |  |  |  |  | 10 |  |
| `walker_rw1_model` | walker | 4 | 410 |  |  |  |  |  |  | 10 |  |
| `walker_rw1_model_naive` | walker | 304 | 607 |  |  |  |  |  |  | 10 |  |
| `bayesdfa_dfa` | bayesdfa | 25 | 416 | 1.04 | 1.04 | 0.40 | 0.40 | 0.28 | 0.28 | 10 |  |
| `bayesdfa_dfa_gp` | bayesdfa | 13 | 0 | 0.57 | 0.91 | 0.71 | 0.71 |  |  | 10 |  |
| `empty_return_shape` | regressions | 3 | 4 |  |  |  |  |  |  | 10 |  |
| `partition_shared_store` | regressions | 2 | 18 |  |  |  |  |  |  | 10 |  |
| `rbmi_mmrm` | regressions | 4 | 15 |  |  |  |  |  |  | 10 |  |
| `reject_udf_data_size` | regressions | 1 | 1 | 0.31 | 0.31 | 0.32 | 0.32 | <0.01 | <0.01 | 10 |  |
| `tuple_array_params` | regressions | 54 | 109 |  |  |  |  |  |  | 10 |  |
| `tuple_coef_cont` | regressions | 40 | 49 |  |  |  |  |  |  | 10 |  |
| `tuple_decompositions` | regressions | 9 | 30 |  |  |  |  |  |  | 10 |  |
| `tuple_decompositions_more` | regressions | 3 | 68 |  |  |  |  |  |  | 10 |  |
| `tuple_effect_once` | regressions | 1 | 4 |  |  |  |  |  |  | 10 |  |
| `tuple_loops` | regressions | 6 | 96 |  |  |  |  |  |  | 10 |  |
| `tuple_reduce_sum` | regressions | 4 | 5 |  |  |  |  |  |  | 10 |  |
| `tuple_return_slices` | regressions | 4 | 100 |  |  |  |  |  |  | 10 |  |
| `while_counter_size` | regressions | 7 | 8 | 0.29 | 0.29 | 0.05 | 0.05 | 0.03 | 0.03 | 10 |  |
| `while_udf_slices` | regressions | 6 | 6 | 0.18 | 0.18 | 0.15 | 0.10 | <0.01 | <0.01 | 10 |  |
<!--/gen-->

### The local corpora

CTest replays seven smaller corpora with the same tool and the same rule
(`tools/verify_refs.py --corpus NAME`). We record their references from
CmdStan in the same way.

| corpus | models | CTest |
| --- | --- | --- |
| `functions` | [10 models that call 468 function names](tests/function_coverage/README.md), including data-only helpers, RNGs, solvers and five operators | `test_function_models` |
| `signatures` | 74 generated models that cover 19,642 signatures, with one output for each case | `signature_refs_*` |
| `sized_functions` | 5 models that call library functions at real sizes | `test_sized_functions_reference` |
| `container_flow` | 66 models that write each container shape in each block and read it back | `test_container_flow_reference` |
| `fixtures` | 63 models from `tests/fixtures` | `*_reference` |
| `cmdstan_replay` | 14 models for rewrites that only inlined `--O1` output reaches | `test_cmdstan_replay` |
| `oncobayes2` | the 3 OncoBayes2 models | `test_oncobayes2_reference` |

CTest runs `container_flow`, `cmdstan_replay` and `oncobayes2` a second time
with the O1 passes of the MIR fixtures. Those passes inline functions, as
stock stanc's O1 does. Those are the `_fixture_passes` CTests and the
`--fixture-passes` rows below.

These are all the values in the local corpora that have a ledger entry:

<!--gen:testing_local_ledger-->
| corpus | model | value | limit |
| --- | --- | --- | --- |
| `container_flow` | `container_flow_rt_fn_amm` | `lp` | 17 (stanli closer) |
| `container_flow --fixture-passes` | `container_flow_rt_fn_amm` | `lp` | 17 (stanli closer) |
| `fixtures` | `nested_callback_contexts` | `grad[2]` | 16 (undecided) |
| `fixtures` | `nested_callback_contexts_fallback` | `grad[2]` | 16 (undecided) |
| `oncobayes2` | `oncobayes2_blrm_exnex_combo3` | `wa beta_EX_prob.2.2` | 14 (undecided) |
| `oncobayes2 --fixture-passes` | `oncobayes2_blrm_exnex_combo3` | `wa beta_EX_prob.2.2` | 14 (undecided) |
| `signatures` | `builtin_signatures_20` | `lp` | 20 (stanli closer) |
| `signatures` | `builtin_signatures_24` | `lp` | 38 (stanli closer) |
<!--/gen-->

We compare a signature model's log density with the exact sum of its recorded
terms. CmdStan adds 360 to 1,440 terms in order there, and its sum has
rounding error of its own.

<a id="checks-run-before-and-after-merge"></a>

## The checks

The tables below use these words:

- "PR" means a pull request that changes source. CI builds and tests it on
  Linux x86-64 with Clang and on Windows x64.
- "Post-merge" means a push to `main`, a nightly run, a release tag or a
  manual run. These add macOS arm64, macOS x86-64, Linux aarch64 and a GCC
  build on Linux x86-64.
- "CTest" means that the check is a CTest and runs wherever the suite runs.

A pull request that touches only documentation runs only the static checks. A
pull request that touches only the R package runs the static checks and the R
tests. To run the post-merge jobs on a branch, use
`gh workflow run wheels.yml --ref BRANCH`. The jobs are in
[`.github/workflows`](.github/workflows).

#### Against CmdStan

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [corpus replay](#every-model) | log density, gradient and `write_array` rows of <!--gen:testing_models-->415<!--/gen--> models at 3 points | recorded CmdStan values, under [the rule](#the-rule) | PR on Linux x86-64, post-merge on macOS arm64 and x86-64, Linux aarch64 and Linux GCC | parameter values away from the 3 points, anything on Windows, and the 10 ULP rule on any platform but macOS arm64 |
| [fast-mode replay](#fast-mode) | the same models compiled in fast mode | the same references, fast-mode limits | PR and post-merge, Linux x86-64 only | macOS, Windows and ARM |
| [local corpus replays](#the-local-corpora) | log density, gradient and outputs of the function, signature, fixture and other local models | recorded CmdStan values | PR (CTest) | signatures and sizes the generators exclude |
| [stanc3 good models](#stan-language-coverage) | the 1,382 models in stanc3's `good` integration tests at up to 5 points | recorded CmdStan values and messages, within 1e-9, with a ledger of known differences | PR (CTest) | models whose generated data CmdStan rejects |
| [stanc3 bad models](#stan-language-coverage) | compile errors of the 1,200 models in stanc3's `bad` integration tests | stanc3's expected output, with a ledger of known differences | PR (CTest) | |
| [generated conformance sweep](#stan-language-coverage) | 24,322 generated signature and construct cases, and each case's status | BridgeStan built from the same pins, and the checked-in baseline | nightly | behavior that only operators use, and cases that the generator cannot build |
| [kernel unit tests](#unit-tests) | one operation's value and every gradient component | the stan-math `var` call CmdStan's generated C++ would make, with the limit stated at each assertion | PR (CTest) | whether lowering selects that kernel |
| [lowering and pass tests](#unit-tests) | log density, gradient, `write_array` rows and op counts of small compiled models | stan-math `var` references, hand-derived constants or the MIR interpreter | PR (CTest) | models outside the fixtures |
| [lit](#unit-tests) | each marked `.stan` file's compile or evaluation result, and optionally one pass dump | the expectation written in the file | PR (CTest) | values the file does not state |
| [fused-density differential](#unit-tests) | the seven fused densities over activity masks, shapes and edge values | Stan Math's kernels, in every bit, or 2 ULP for normal and cauchy | Linux x86-64 on PRs and post-merge, and the AddressSanitizer and Windows ARM64 jobs | macOS |
| BridgeStan C ABI | 4 fixture models: values, names, counts, shapes | reference BridgeStan | post-merge | other models |
| WebAssembly replay | log density and gradient of the corpus through the wasm build | the same references and limits | by hand | outputs, and `nn_rbm1bJ100`, whose compile does not fit in 4 GB |

#### stanli against itself

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [cross-path matrix](#comparing-stanlis-execution-paths) | every fixture at 3 points and the 22 cogmod models at 13, under 19 execution configurations | each other, in every bit, with a ledger | PR on Linux x86-64, in the build with developer switches. Post-merge under AddressSanitizer and on Windows ARM64 | transformed data, which has one engine, and an error that every path shares |
| [posteriordb cross-path](#comparing-stanlis-execution-paths) | the posteriordb models under the same configurations plus one pass-off set | each other, within 1e-11 | by hand after pass changes | an error present in both cells |
| [compiler producer parity](#stan-language-coverage) | compact MIR bytes from the native, JavaScript and Windows (`stanli-compile.exe`) compilers | each other, byte for byte | PR | numerical behavior |
| [interpreter oracle](#the-interpreter-oracle) | every fixture, lit case and regression model at 5 points: log density, gradient at 2 of them, `write_array`, printed output and errors | the MIR interpreter on `--passes none` MIR, with a ledger | PR (CTest) | solver models below the solver's tolerance, gradients of models with more than 200 parameters, and a bug that the interpreter shares |
| Rethinking regeneration | the 62 Rethinking fixtures, regenerated from the book's code with pinned `rethinking` | the checked-in files, byte for byte | nightly, and pushes to `main` that touch them | |

#### Samplers and algorithms

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [sampler parity](#samplers-and-other-algorithms) | NUTS draws and statistics across metrics and options | Stan's own NUTS services on the same executor, in every bit | PR (CTest) | compiler numerics, because both sides share the executor |
| [algorithm checks](#samplers-and-other-algorithms) | optimize, Laplace, Pathfinder, ADVI, fixed_param, RNG streams | a known Gaussian posterior, and Stan's generator | PR (CTest) | non-Gaussian posteriors |
| [reference posteriors](#samplers-and-other-algorithms) | NUTS means and standard deviations of the 39 posteriordb models with reference draws | posteriordb reference draws, within 4 Monte Carlo standard errors | by hand | WALNUTS, and models without reference draws |
| [sampler trace](#samplers-and-other-algorithms) | step counts, tree depth, step size, divergences, mean `lp__` | CmdStan with the same settings | by hand after sampler changes | statistical validity of NUTS |
| [sampling smoke](#every-model) | 100 saved draws of the 13 Aalto fixtures | reference output names, draw count, finiteness | PR (CTest) | convergence |
| [brms end to end](#samplers-and-other-algorithms) | `brm()` fits of the 124 brms formulas with the `stanli` backend | the same fits with brms's `cmdstanr` backend | by hand | anything beyond posterior means, and platforms other than macOS arm64 |

#### Packages, platforms and memory

| check | compares | against | when | cannot see |
| --- | --- | --- | --- | --- |
| [installed clients](#installed-clients) | the built wheel, R package and BridgeStan transport | API tests and R ecosystem consumers. A skip fails | PR: wheel on Linux x86-64 and Windows x64, R on Linux. Post-merge: wheel on macOS and Linux aarch64, R ecosystem subset on macOS arm64 and Windows x64 | the R suite with a runtime on macOS and Windows |
| browser runtime | `tests/test_wasm.cjs` and `tests/test_worker_diagnostics.cjs`: compile, evaluate and sample Eight Schools through the browser build and its worker | expected values and API behavior | pushes to `main`, and `test_wasm.cjs` again on release tags | pull requests and nightly runs |
| webR load | the WebAssembly side module | webR's loader and the C API symbols | post-merge | the R package flow |
| downstream `stanr` | `stanr` built against this runtime, its backend tests | its own expectations | on demand only | other R consumers |
| no-stdio build | undefined symbols of the runtime objects (via [`tools/check_no_stdio.sh`](tools/check_no_stdio.sh)) | the stdout, `printf`, `abort` and `exit` symbols `R CMD check` rejects | post-merge | |
| Windows dev setup | [`tools/dev_setup.sh`](tools/dev_setup.sh) on Windows, then CTest | the full suite on ARM64 (through `stanli-compile`), and six tests on x64 with the embedded compiler | pushes to `main`, on demand | the rest of the suite with the embedded compiler on x64 |
| AddressSanitizer | the CTest suite, compiling through `stanli-compile`, with UBSan's signed-integer-overflow check as well | no diagnostics | post-merge except release tags | a stale `vari` that points into reused arena memory, and undefined behavior other than signed overflow |
| input fuzzers ([`tests/fuzz`](tests/fuzz/)) | libFuzzer on the `whiten` member of the BridgeStan manifest, the sizes and arrays given to the low-rank whitening and sparse-factor functions, and data JSON, under ASan and the signed-overflow check | no crash or diagnostic | nightly, Linux x86-64, 5 minutes per target. Every CTest run replays the seed corpora | inputs that take longer than 5 minutes to reach |
| ThreadSanitizer | `test_capi`, `test_pool`, `test_multichain`, `test_wa_parallel`, `test_wa_parallel_abi` and `test_wa_parallel_switches` | no diagnostics | post-merge except release tags | the other tests |
| bundled compiler | `r/inst/js/stanc.js`, and the compiler loaded in a browser worker, in webR and through V8 in R | a fresh build, byte for byte, and expected compile results | PR | |
| Windows compiler | `stanli-compile.exe`, the Linux cross-build of the compiler for Windows (the release links the embedded compiler instead and does not ship it) | expected results in `tests/test_windows_compiler.R`, and no integer folded past int32 | PR | |
| R ABI | the ABI constants in the runtime and in the R bridge | each other | PR and pushes that touch the R paths | |
| `R CMD check` matrix | the R package without a runtime, on 4 OS and R combinations, warnings as errors | R's own checks | post-merge | sampling, which skips without a runtime |
| packaging guards | glibc symbol versions, the macOS minimum version, the x86-64 instruction set of the Linux library | the wheel tag and the baseline ISA | every build | |
| signature watch | the signatures of upstream stanc3 | the signatures in the conformance baseline | nightly. It reports and does not fail | |
| Safari 17 bundle check | the js_of_ocaml bundles | no line ending in `static` | PR | |
| docs and formatting | stamped numbers, C/C++ formatting | their artifacts ([`tools/gen_docs.py --check`](tools/gen_docs.py)), the pinned formatter | every PR | |

<a id="installed-clients"></a>
The wheel jobs run `tests/test_python.py`, `tests/test_bridgestan_embed.py`
and six Laplace whitening and annotation tests on the installed wheel. The
Linux R job runs `R CMD check` with the runtime and then
[`tools/test_r_ecosystem.R`](tools/test_r_ecosystem.R). That script tests
bayesplot, bridgesampling, loo, posterior, stanfit and the registration of
the backend. It fails on any skip, failure, error or warning. The macOS and
Windows R jobs run only that script.

Two checks measure time and compare no numbers. They are
`test_prep_scaling` and `test_prep_deep_expression_time`. Release tags also
check the version strings, the runtime tarballs and the PyPI description.

The tables leave out a few things:

- the tests of CI's own scripts, such as `test_ci_policy` and
  `test_dev_setup`
- two measurements that cannot fail a run: the R time to first posterior, and
  a webR canary that only warns
- four harnesses that CI never runs: `fn_sweep.py`, `ode_sweep.py`,
  `transform_sweep.py` and `collapse_census.py` in [`harnesses`](harnesses)

The sections below say more about some of the checks.

### Recording and replaying the references

[`tools/verify_sample.py`](tools/verify_sample.py) records the references. It
uses CmdStan's own driver ([`tools/ref_driver.cpp`](tools/ref_driver.cpp))
with `propto=true` and the Jacobian. It seeds Stan's RNG the same way that
stanli does, so we can also compare generated quantities that draw random
numbers.

[`tools/verify_refs.py`](tools/verify_refs.py) replays the references and
does not need CmdStan. The replay fails if a point is missing, or if a model
or data file changed. It also fails if someone recorded a reference with a
CmdStan, Stan or Stan Math version other than the pinned one.

Linux x86-64 has its own recording of the brms models, and a second one for
GCC. A CmdStan gradient can differ by hundreds of ULP between platforms and
compilers. The replay chooses the recording from the platform and compiler of
the binary that it tests. It chooses before it evaluates anything.

`tools/verify_refs.py` has three lists that can excuse a failure:

- `KNOWN_GAPS`, for a model that stanli refuses
- `QUARANTINED`, for a point with an open bug
- `CORPUS_INTERPRETED`, for a model that runs through the interpreter

All three are empty today. If a listed model starts to pass, the run fails
until someone deletes the entry.

### Unit tests

A kernel is the runtime's implementation of one operation. Each kernel test
makes the call that CmdStan's generated C++ would make with
`stan::math::var`. It compares the value and every gradient component.

[`tests/expect.hpp`](tests/expect.hpp) has the assertions. `expect_eq`
compares every bit, so `+0` and `-0` differ. `expect_ulp` takes a budget as an
argument. Most comparisons use `expect_eq`. The largest budget in a kernel
test is 16 ULP, for the adjoints of the matrix solves. The largest budget in a
lowering test is 40 ULP, for a chain of matrix pullbacks.

stanli adds up `sum()` and `mean()` of more than 32 values with compensation.
It adds up the sums that it builds from a loop's terms pairwise. Stan Math
adds in order. [`tests/test_sum_accuracy.cpp`](tests/test_sum_accuracy.cpp)
compares stanli's sums with the exact sum of the same doubles. These sums are
why stanli is closer to the true value in most ledger entries.

Seven common densities have fused kernels. A default CMake build also keeps
the Stan Math code that they replace.
[`tests/test_density_fused.cpp`](tests/test_density_fused.cpp) compares the
two over every activity mask, shape, edge value and rejection message. Normal
and cauchy may differ by 2 ULP. The other five must match in every bit.

[`tests/test_pass_safety.cpp`](tests/test_pass_safety.cpp) compares gradients
before and after the graph passes on 400 random graphs.

When we add a regression test, we first check that it fails against the
source from before the fix.

### Comparing stanli's execution paths

stanli can run a model in three ways: graph lowering, the MIR interpreter and
the register program. Each one implements Stan's operations separately, so
one can be wrong while another is right.

[`tests/cross_path.hpp`](tests/cross_path.hpp) compiles a model once for each
of 19 configurations and compares the results at three points. Every bit must
match. [`tests/cross_path_ledger.json`](tests/cross_path_ledger.json) lists
the exceptions, each with a cause and a bound in ULP. A full run fails if an
entry excuses nothing.

The ledger has <!--gen:testing_cross_entries-->64<!--/gen--> entries. Most allow 1 to 10 ULP
for sums that the region map and the lane passes add in a different order.
Two `cm_rdm` entries allow 2,526 ULP on Windows for a rounding difference
there.

Another <!--gen:testing_cross_unbounded-->10<!--/gen--> entries allow any distance. They cover
one open bug. On Windows x86-64, a gradient entry of
`higher_order_all_contexts` and one of `ode_adjoint_region` were far off in
some runs. We do not know the cause. One of these entries names no platform,
so it allows any distance on every platform.

The matrix runs over every fixture, and over the 22 cogmod models at 13
points. It needs [developer switches](#developer-switches), so it runs only
in a build that has them.

<a id="mir-loop-vectorization-measurement"></a>
After you change a pass, run `stanli_check --cross --cross-corpus
--cross-rel-tol 1e-11` over posteriordb by hand.
[`runtime/src/OPTIMIZATIONS.md`](runtime/src/OPTIMIZATIONS.md) has the steps.

### The interpreter oracle

`stanli_check --interp` runs a whole model through the MIR interpreter on
doubles. It takes the gradient by finite differences and prints an error
estimate for each component on a `GRAD_ERR` line. The interpreter reads
the MIR directly and does not use the graph passes or kernels of the
compiled path.

[`tools/check_interp.py`](tools/check_interp.py) runs each model in
`tests/fixtures`, `tests/lit` and `tests/regressions` both ways. The
compiled path reads the shipping MIR and the interpreter reads the
`--passes none` MIR, so a wrong stanc3 pass also shows. It compares the log
density, the `write_array` row, printed output and error messages at five
points, and the gradient at points 3 and 4. Values agree within 1e-10
relative. A gradient component agrees within 10 times its error estimate
plus 1e-6 relative.

[`tests/interp_ledger.json`](tests/interp_ledger.json) lists every model
whose lp was not compared and agreed at one point or more, in groups that
share a state and a cause. Most entries are lit cases and fixtures that are
expected to fail, fixtures that have no data file, models that the compiled
path refuses while it prepares them, and ODE models that differ by the
solver's tolerance. A model whose state differs from its entry fails the
test, in both directions: an entry for a model that now agrees must be
removed, and a model that starts to fail the same way on both sides needs
one. A model in a `skipped` group does not run. After an intended change,
run `python3 tools/check_interp.py --update-ledger` and replace each
`UNTRIAGED` cause with the reason. The test prints how many models it
compared on lp, on the gradient and on write_array rows.

The CTest entry is `test_interp_oracle`. It takes about 11 seconds with 4
jobs. No state depends on the machine's speed: a model that times out fails. To run another corpus by hand, pass `--out DIR` and `--corpus` with
`posteriordb`, `brms`, `stanc3_good`, `dir:PATH` or one of the others that
`--help` lists.

The oracle has limits:

- For models with a solver (ODE, DAE, algebra, `integrate_1d`), the solver
  on doubles and the solver on vars differ by about the solver's tolerance.
  Such a model gets no gradient comparison, and its lp difference is
  ledgered.
- A model with more than 200 free parameters gets no gradient comparison.
- A finite difference cannot see a gradient error much smaller than about
  1e-5 relative. A model where Stan Math's derivative differs from the
  finite difference of its value is ledgered as `gradient_only`.
- The interpreter is slow on large models: ctsem takes about 90 seconds, so
  such models only run by hand.

### Fast mode

Fast mode (`fast_math`, see [docs/fast-mode.md](docs/fast-mode.md)) may add up
sums in a different order and replace arithmetic over observations with closed
forms. The 10 ULP rule does not apply to it.
`tools/verify_refs.py --fast-math` replays the same points against the same
CmdStan references. At each point:

- The log density must be within 1e-12 relative error.
- Take the largest gradient difference and divide it by the largest gradient
  entry, or by 1 if that is larger. The result must be below 1e-12, which is
  about 4,500 ULP of the largest entry.
- Rejections and non-finite values must match.
- Outputs keep the 1e-9 limit.

One model has a wider limit. `FAST_HP_CHECKED` allows
`epinow2_estimate_infections` 1e-10, because an 80-digit evaluation shows that
fast mode is about as close to the true value as CmdStan there. The
ill-conditioned models and the known mismatches keep the limits from the big
table.

[`tests/test_collapse.cpp`](tests/test_collapse.cpp) compares every graph that
fast mode rewrites with the graph it came from.
[docs/fast-mode.md](docs/fast-mode.md#how-accurate) has the measured accuracy.

### Samplers and other algorithms

A gradient at a fixed point cannot show a wrong sampler setting. These checks
can:

- [`tests/test_sampler_parity.cpp`](tests/test_sampler_parity.cpp) runs one
  executor through stanli's NUTS driver and through Stan's own `hmc_nuts_*`
  services, which CmdStan calls. Every saved row must match in every bit. The
  cases cover the three metrics, adaptation on and off, a supplied metric and
  step size, thinning, saved warmup and chain ids.
- [`tests/test_capi_algorithms.cpp`](tests/test_capi_algorithms.cpp) runs
  optimize, Laplace, Pathfinder, ADVI and fixed_param on a correlated
  Gaussian. The mode must be within 1e-4. The other algorithms must recover
  the means and standard deviations within five standard errors.
- [`tools/verify_posteriors.py`](tools/verify_posteriors.py) samples the 39
  posteriordb models that have reference draws, with 4 chains of 1000
  iterations. A model fails when a mean or standard deviation is more than 4
  Monte Carlo standard errors from the reference. We run it by hand.
- [`tools/sampler_trace.py`](tools/sampler_trace.py) compares leapfrog counts,
  tree depth, step size and divergences with a CmdStan run of the same model
  and seed. It finds large configuration errors. We run it by hand.
- [`harnesses/brms_compare_backends.R`](harnesses/brms_compare_backends.R)
  fits each brms formula with the `stanli` and `cmdstanr` backends and
  compares posterior means. We run it by hand. The
  [latest recording](output/brms-end-to-end/README.md) has the results.

The Python and R tests check only that each binding passes arguments, results
and errors through.

<a id="testing-stan-language-coverage"></a>

### Stan language coverage

[`harnesses/stan_conformance.py`](harnesses/stan_conformance.py) reads every
function signature that the pinned compiler knows, and a catalog of language
constructs. It generates a small model for each one. It evaluates the model
with stanli and with BridgeStan built from the same pins, and compares the log
density and gradient. The nightly run fails if a case that used to pass stops
passing. [docs/coverage.md](docs/coverage.md) summarizes the results by
function family.

stanc3's own integration tests cover syntax and type combinations that real
models do not use. [`tools/check_stanc3_good.py`](tools/check_stanc3_good.py)
compares the `good` models with recorded CmdStan values and messages.
[`tools/check_stanc3_bad.py`](tools/check_stanc3_bad.py) compares the error
text of the `bad` models with stanc3's expected output. Each tool has a ledger
of known differences. A model fails if it does better or worse than its ledger
entry. [`tests/stanc3_good/README.md`](tests/stanc3_good/README.md) has the
details.

The native, JavaScript and Windows (`stanli-compile.exe`) compilers must emit the same bytes for the
[compiler fixtures](tests/compiler/README.md).

### Developer switches

About 60 `STANLI_*` environment variables turn a pass off or force an engine.
We call them developer switches. Shipped builds compile them out
(`STANLI_DEBUG_SWITCHES=OFF`).

A test that needs a switch has the CTest label `debug_switches`. In a shipped
build, such a test skips the cases that need the switch. CMake disables the
cross-path matrix and eight other tests there, because almost all of their
cases need one.

These tests run in builds that have the switches. For pull requests, that is a
second build on Linux x86-64. After a merge, the AddressSanitizer job and the
Windows ARM64 job also have them. No CI job runs these tests on macOS.
[docs/hacking.md](docs/hacking.md#tests-that-need-developer-switches) lists
the tests.

## Known limits

### CI enforces the 10 ULP rule on one platform, after merge

The ledger has measurements for macOS arm64 only. A pull request can move a
value by more than 10 ULP and still merge, if the value stays within 1e-9 and
is not in a brms model or the `fixtures` corpus. The macOS run after the merge
then fails.

The values with true values are the exception: Linux x86-64 checks them in
pull requests. We measured their bounds on macOS arm64 only, so a bound near
10 leaves little room for another platform's libm.

For a few entries, stanli is farther from the true value than from CmdStan,
so the check against the true value allows more than the CmdStan bound did.
In the largest case, `epinow2_estimate_infections` grad[17], both engines
are about 8,400 ULP from the true value.

To close this gap, someone needs to record a ledger section on Linux x86-64.
After that, the separate brms limit is no longer needed.

### stanli is less accurate than CmdStan for some values

In <!--gen:testing_worse_models-->35<!--/gen--> models, <!--gen:testing_worse_values-->246<!--/gen--> values are
more than 10 ULP from the true value in stanli and farther from it than in
CmdStan.

In nine of those models, stanli is more than 5 ULP farther than CmdStan. These
issues track them:
[#643](https://github.com/seantalts/stanli/issues/643),
[#644](https://github.com/seantalts/stanli/issues/644),
[#645](https://github.com/seantalts/stanli/issues/645),
[#646](https://github.com/seantalts/stanli/issues/646),
[#647](https://github.com/seantalts/stanli/issues/647),
[#648](https://github.com/seantalts/stanli/issues/648),
[#649](https://github.com/seantalts/stanli/issues/649) and
[#650](https://github.com/seantalts/stanli/issues/650).

In the other models, both engines are far from the true value and stanli
differs from CmdStan by a ULP or two.
[#651](https://github.com/seantalts/stanli/issues/651) lists them.

### Some values have no 80-digit value

The accuracy columns are empty where the 80-digit evaluator cannot score a
value. For those values, the only evidence is the distance from CmdStan.

### We test three points for each model

Agreement at three points does not prove agreement over the whole parameter
space. The sampler checks cover a small part of that gap.

### Two models have a known mismatch

Two gradient entries of `kronecker_gp` differ from CmdStan by 0.7%. They go
through `eigenvectors_sym` of a covariance matrix whose two closest
eigenvalues are 6.5e-17 apart. The derivative of an eigenvector grows as that
gap shrinks, so a difference in the last bit of the input becomes a large
difference in the output.

Three gradient entries of `stanguide_ode_adjoint` differ from CmdStan by up to
4e-6. They come from an adjoint ODE solve with a tolerance of 1e-6. When we
set the tolerance to 1e-11, the two engines agree to 1e-10.

### Some models have nothing to compare

CmdStan and stanli both reject every point of the posteriordb model `sir` and
of the brms model `s2_invgaussian`.

Another <!--gen:testing_models_no_wa-->6<!--/gen--> models have no recorded outputs. For those
models the replay checks that stanli writes a row and does not check the
values in it. The big table shows 0 in their outputs column.

### Other gaps

- Windows never runs the main corpus. It runs CTest, which includes the local
  corpora.
- One cross-path bug is open on Windows x86-64. See
  [Comparing stanli's execution paths](#comparing-stanlis-execution-paths).
- CI builds the WebAssembly runtime and runs Eight Schools through it. We run
  the corpus through it by hand, and that run does not check outputs.
- CI replays fast mode on Linux x86-64 only.
- The `cmdstan_replay` references do not record a CmdStan revision. A change
  of pin does not fail that corpus.
- Nothing tests the R interrupt path.
- The data-loop memo has one test,
  [`tests/test_data_loop_memo.py`](tests/test_data_loop_memo.py), and no
  cross-path fixture.
- A new release can move a value by its last bits, within the rule. The
  release notes give the largest distance that we measured.
