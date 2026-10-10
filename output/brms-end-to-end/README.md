# brms end to end: `brm()` with the stanli and rstan backends

Recorded 2026-10-09. Every formula in `tools/gen_brms_models.R` (the 124
brms model shapes of the corpus) was fitted through `brm()` twice: with a
`stanli` backend, and with brms's default `rstan` backend. `manifest.json` has
the package versions, the sampler configuration and the stanli commit.

The `stanli` backend is a local branch of brms (`backend-stanli`, on brms
`c89c4c05`); it is not in any brms release. It needs the development version
of the stanli R package from this repository.

## Result

- All 124 formulas fit with both backends, and both store the same variables
  for every one.
- 103 models have rhat below 1.01 on every variable in both backends. On
  those, over 1,414 variables, the largest difference between the two
  backends' posterior means is 3.4 combined Monte Carlo standard errors
  (`i319_negbin_re`); the median over models of the largest difference is 1.4
  and the 95th percentile 2.8.
- Of the other 21, 11 exceed 1.01 with both backends (`sw_negbinomial`,
  `sw_re_negbin`, `sw_mixture`, `s2_mixture_theta` and `sw_vonmises` do not
  converge with either), 6 only with stanli and 4 only with rstan, each of
  those ten between 1.01 and 1.04. The data are 40 simulated rows; the
  comparison says nothing about these models.
- `summary()`, `as_draws_df()`, `posterior_predict()` and `log_lik()` succeed
  or fail the same way with both backends on every model. The failures are
  brms's own: a custom family without R functions, `posterior_predict()` for
  `cox`, a missing optional package.

What this does not show: agreement beyond posterior means; any formula not in
the list; any platform but Linux x86-64.

## Files

- `stanli.tsv`, `rstan.tsv`: one row per formula from
  `harnesses/brms_end_to_end.R`: whether `brm()` returned a fit, wall seconds,
  largest rhat, divergent transitions, and whether four post-processing calls
  succeeded.
- `compare.tsv`: from `harnesses/brms_compare_backends.R`, with stanli as `a`
  and rstan as `b`.
- `manifest.json`.

The seconds are not a benchmark. rstan's include compiling each model, and
the rstan fits ran in up to eight processes at once while stanli's ran in
one. For scale only: 250 seconds in total for stanli (median 0.4 per model)
and 7,061 for rstan (median 53).

## Reproducing

```sh
Rscript harnesses/brms_end_to_end.R stanli stanli.tsv
Rscript harnesses/brms_end_to_end.R rstan rstan.tsv   # needs a C++ toolchain
Rscript harnesses/brms_compare_backends.R stanli.tsv rstan.tsv compare.tsv
```

The comparison reads the `*.summaries.rds` files the first script writes
beside each table; they are not kept here.
