# brms end to end: `brm()` with the stanli and cmdstanr backends

Recorded 2026-10-21. Every formula in `tools/gen_brms_models.R` (the 124
brms model shapes of the corpus) was fitted through `brm()` twice: with the
`stanli` backend, and with brms's `cmdstanr` backend on CmdStan 2.40.0, the
Stan version stanli is pinned to. `manifest.json` has the package versions,
the sampler configuration and the stanli commit.

The `stanli` backend is the branch
[`stanli-backend`](https://github.com/seantalts/brms/tree/stanli-backend) of
brms, one commit on brms's current master; it is not in any brms release.

## Result

- All 124 formulas fit with both backends, and both store the same variables
  for every one.
- 102 models have rhat below 1.01 on every variable in both backends. On
  those, over 1,432 variables, the largest difference between the two
  backends' posterior means is 3.4 combined Monte Carlo standard errors
  (`i319_pois_re2`); the median over models of the largest difference is 1.5
  and the 95th percentile 2.5.
- Of the other 22, 10 exceed 1.01 with both backends (`sw_negbinomial`,
  `sw_re_negbin`, `sw_mixture`, `s2_mixture_theta` and `sw_vonmises` do not
  converge with either), 7 only with stanli and 5 only with cmdstanr. Of
  those twelve, ten are between 1.01 and 1.04. The other two are stanli's
  `sw_re_pois` (1.20) and `s2_categorical_re` (1.11), each with one chain
  stuck at this seed. Over six other seeds both backends gave rhat between
  1.002 and 1.026 on both models, with similar divergence counts. The data
  are 40 simulated rows; the comparison says nothing about these models.
- `summary()`, `as_draws_df()`, `posterior_predict()` and `log_lik()` succeed
  or fail the same way with both backends on every model. The failures are
  brms's own: a custom family without R functions, `posterior_predict()` for
  `cox`, a missing optional package, `posterior_predict()` without `resp` for
  a multivariate model with `subset`.

What this does not show: agreement beyond posterior means; any formula not in
the list; any platform but macOS on Apple silicon. The previous recording
(2026-10-09, Linux x86-64, against rstan) found the same.

## Files

- `stanli.tsv`, `cmdstanr.tsv`: one row per formula from
  `harnesses/brms_end_to_end.R`: whether `brm()` returned a fit, wall seconds,
  largest rhat, divergent transitions, and whether four post-processing calls
  succeeded.
- `compare.tsv`: from `harnesses/brms_compare_backends.R`, with stanli as `a`
  and cmdstanr as `b`.
- `manifest.json`.

The seconds are not a benchmark. cmdstanr's include compiling each model, and
the cmdstanr fits ran in eight processes at once while stanli's ran in one.
For scale only: 158 seconds in total for stanli (median 0.3 per model) and
896 for cmdstanr (median 5.4).

## Reproducing

```sh
Rscript harnesses/brms_end_to_end.R stanli stanli.tsv
Rscript harnesses/brms_end_to_end.R cmdstanr cmdstanr.tsv   # needs CmdStan
Rscript harnesses/brms_compare_backends.R stanli.tsv cmdstanr.tsv compare.tsv
```

The cmdstanr run here was split with `BRMS_E2E_SHARD=k/8` and the eight tables
and `*.summaries.rds` files joined afterwards. The comparison reads the
`*.summaries.rds` files the first script writes beside each table; they are
not kept here.
