# Fast mode sampling check: do posteriors agree with default mode?

Status: measurement record, 2026-10-09, `origin/main` at `62375382`, release
build on Apple Silicon (arm64). Harness:
[`harnesses/fast_sampling_check.py`](../../harnesses/fast_sampling_check.py),
tests in [`tests/test_fast_sampling_check.py`](../../tests/test_fast_sampling_check.py).
Outputs are in [data/2026-10-09-fast-mode-sampling/](data/2026-10-09-fast-mode-sampling/).
This closes the sampling-checks item in
[numerics versus speed](2026-10-05-numerics-vs-speed.md).

## Result

- 120 corpus models were sampled in both modes: the 39 with a posteriordb
  reference posterior plus 81 more that fast mode changes the most (the
  observation collapse, or the largest log density and gradient deviations).
  All 240 runs finished; none timed out or was skipped.
- 103 models show no difference at the stated thresholds. 17 were flagged.
  Re-running the 17 with four more seeds and one 4,000-draw run, and sampling
  default mode against itself with different seeds, flags default against
  default as often as fast against default. None of the 17 is a fast-mode
  difference. No fast-mode bug was found.
- Against the reference posteriors, fast mode and default mode differ from the
  reference on the same models: `eight_schools_centered` (the reference is the
  noncentered posterior) and `one_comp_mm_elim_abs` (heavy-tailed; the two
  modes produce identical draws). `hmm_drive_0`, `hmm_drive_1` and `mesquite`
  flagged once in five seeds, in one mode or the other.
- ESS per gradient evaluation, fast over default, is 1.01x in geometric mean
  over the 120 models (bootstrap 95% interval 0.95 to 1.09; median-bulk
  version 1.04x, 0.98 to 1.13). Across seeds, the value for one of the 17 flagged models moves by a
  median factor of 1.21 in default mode alone, so no single model's ratio
  means anything.
- At the 1,056 recorded points of the replay corpus, log density and gradient
  of fast mode against CmdStan are within 1e-12 for all 352 models (worst
  `election88_full`, 1.6e-13), from `tools/verify_refs.py --fast-math`. That
  is the same-point density and gradient comparison; no flagged model needed
  a further one.

## Method

Each model is sampled with `build-rel/stanli_run`, default and `--fast-math`,
with the same settings: 4 chains, 1,000 warmup and 1,000 draws, `delta` 0.8,
maximum tree depth 10, seed 20261009, `--sampler-stats --save-warmup`. Up to
8 sampling processes ran at once with 4 threads each, 30 minute limit per run.
No wall times are reported.

The arms are compared as distributions. Two runs diverge after the first
gradient that differs in its last bit, so draws are never paired.

Per model and parameter (generated quantities included), from the draws after
warmup:

- mean, sd, 5% and 95% quantiles, rank-normalized split R-hat, bulk and tail
  ESS, and the MCSE of the mean, sd and quantiles, with the definitions and
  numbers of the R `posterior` package (the test fixture is `posterior`
  1.7.0 output, matched to 6 places, and on the `blr` run the harness agrees
  with `posterior` in every printed digit of R-hat, ESS and MCSE);
- ESS per gradient: the smallest bulk ESS over parameters divided by the
  gradients of the 4,000 post-warmup transitions (summed `n_leapfrog__`);
- per run: divergences, transitions at maximum tree depth, and the stay rate
  (a draw identical to the previous one, all parameters).

Flag rules, all in `compare_arms`:

- Mean, sd (as a log ratio) and quantiles: z is the difference over the
  combined MCSE. With K compared columns there are 4K tests per model, and a
  model is flagged when any |z| exceeds the Bonferroni quantile for family
  error 0.01 (3.3 for 3 columns, 4.7 for 1,000).
- R-hat above 1.01 in the worst column of exactly one arm.
- Divergence, max-tree-depth and stay rates: two-proportion z above 3 and a
  rate difference of at least 0.5 percentage points.

A flagged model is re-run with seeds 20261010 to 20261013 and once with
4,000 draws (seed 777), then judged by two questions: does the flag recur in
one direction, and does default mode flag against itself (seed against seed)
at a similar rate. The second question matters because the rate tests and
MCSE treat the 4,000 draws as one sample. Step size adaptation, and chains
that stall in funnels or between modes, vary from run to run by more than that.

Against a reference posterior, the same mean, sd and quantile tests apply,
over the parameters both have (10 chains of 1,000 draws from posteriordb at
`28f8d3d`). Rate and R-hat flags need sampler output and are not used there.

## Models

| set | models |
| --- | ---: |
| with a reference posterior | 39 |
| observation collapse applies (`collapse_census.py`, a term with no refusal) | 98 |
| largest 30 fast-mode deviations (`verify_refs.py --fast-math`, 8.3e-15 to 1.6e-13) | 30 |
| sampled (union) | 120 |

26 reference models also collapse; no deviation-set model has a reference.
Not sampled: the other 232 of 352 replay models. Eight sampled models give
bitwise identical draws in both modes (`GLMM1_model`, `Survey_model`,
`eight_schools_centered`, `gp_pois_regr`, `gp_regr`, `ldaK2`,
`low_dim_gauss_mix`, `one_comp_mm_elim_abs`), so their comparison is trivially
clean.

## Summary

| comparison | models | no flag | flagged |
| --- | ---: | ---: | ---: |
| fast against default | 120 | 103 | 17 |
| fast against reference | 39 | 35 | 4 |
| default against reference | 39 | 36 | 3 |

Flags in the 17 (a model can raise several): stay rate 8, R-hat 6, max tree
depth 5, divergences 3, and one each of sd, q05 and q95. Mean never flagged
on its own. The largest mean difference among the 103 unflagged is 0.36 sd
(`ch09_m9_4`, R-hat 1.5 and 1.9 in the two modes, so neither run mixed); the
largest among those with R-hat below 1.1 is 0.09 sd. Eleven models have R-hat above 1.01
in both modes; four (`ch09_m9_4`, `hmm_drive_0` default, `ldaK2`,
`normal_mixture_k`) are above 1.1 in some mode, from label switching or
bimodality.

Per-model rows: [main.csv](data/2026-10-09-fast-mode-sampling/main.csv).

## Flagged models and resolutions

"dd", "ff" and "fd" are the number of pairs flagged out of 6, 6 and 16 among
seeds 1 to 4 (default against default, fast against fast, fast against
default; [table](data/2026-10-09-fast-mode-sampling/control-seed-to-seed.md)).

| model | flag in the first run | dd / ff / fd | resolution |
| --- | --- | --- | --- |
| `radon_county`, `radon_variable_slope_centered`, `radon_partially_pooled_noncentered`, `radon_variable_intercept_slope_noncentered` | worst R-hat 1.008 to 1.019, above 1.01 in one mode | 0/0/2, 0/0/1, 3/3/6, 0/3/5 | R-hat sits near 1.01 in both modes, 400 to 1,500 parameters, so the maximum crosses the line by chance, in either direction across seeds. The 4,000-draw run is below 1.005 in all four. |
| `accel_gp`, `ch09_m5_8s` | max tree depth rate (and divergences for `accel_gp`) | 5/6/16, 1/5/12 | Both sit near the depth limit. Over 17 seeds: tree depth hits per run 1,974 against 2,073 (`accel_gp`) and 1,882 against 1,867 (`ch09_m5_8s`); Welch p 0.72 and 0.74; step size p 0.69 and 0.88; divergences in `accel_gp` 63 against 101 (p 0.43). |
| `kilpisjarvi`, `election88_full` | max tree depth rate | 4/3/7, 4/3/10 | Default flags against itself at the same rate. The direction alternates across seeds. |
| `ch13_m13_4`, `ch13_m13_4b`, `ch14_m14_2` | R-hat, divergences, stay, tail quantiles | 5/6/12, 5/6/12, 6/5/14 | Funnel posteriors where a stalled chain (R-hat above 1.1) appears in about half of the runs. Default stalls in the 4,000-draw run (R-hat 1.5, 2,836 and 4,214 divergences) and in seed 2 (`ch13_m13_4`) or seed 1 (`ch14_m14_2`); fast stalls in seed 3 (both) and seed 4 (`ch14_m14_2`). The twins `ch13_m13_4` and `ch13_m13_4b` are the same model and data. |
| `hmm_drive_0` | sd of `lambda[2]` z -10, R-hat 2.6 in default | 0/3/4 | Bimodal. Default stalls in the first run, fast in seed 2 (R-hat 3.2, sd ratio 110); both are clean in seeds 1, 3, 4. |
| `normal_mixture_k` | q05 of `sigma[5]` | 6/6/14 | Label switching, R-hat 1.5 to 2.1 in every run of both modes. The z scores are not meaningful. |
| `s2_mi_trunc_lb` | stay rate | 5/6/12 | Divergent funnel; the stay rate varies with the adapted step size in both modes. |
| `ch15_m15_9`, `s2_custom_vreal`, `sesame_one_pred_a` | stay rate | 0/0/0, 0/0/1, 0/0/0 | Single-seed event; one flagged pair in 16 or none in seeds 1 to 4. |

Against the references:

| model | flag | resolution |
| --- | --- | --- |
| `eight_schools_centered` | tau q05 (4 of 5 seeds), both modes | The reference posterior named in posteriordb is the noncentered model's. The centered funnel's lower tail differs. Same flags in both modes, identical draws. |
| `one_comp_mm_elim_abs` | sd of `V_m`, `K_m` (4 of 5 seeds), both modes | The reference has sd 7.3 for `K_m` against 2.5 to 4.1 in our 4,000-draw runs; the posterior is heavy-tailed and the runs' MCSE of the sd understates it. Both modes give identical draws, so this is a default-mode property. |
| `hmm_drive_0`, `hmm_drive_1`, `mesquite` | sd or q05, one of five seeds each | `hmm_drive_0` flags in default in the first run and in fast in seed 2, the bimodality above. `hmm_drive_1` flags in both modes in different seeds. `mesquite` (q05 of `beta[2]`) flagged once in fast and never in default. It does not recur in four more seeds. |

## ESS per gradient

Fast over default, minimum bulk ESS over parameters per post-warmup gradient:
geometric mean 1.01 over 120 models (1.01 over the 112 whose draws differ),
median 1.00 (1.01 over the 112); 47 models above 1.02 and 51 below 0.98. Tail ESS: 1.03. Median
bulk ESS over parameters: 1.04. Gradients per run: 0.99. Warmup gradients:
1.01. The spread is wide (0.28 to 29 for the minimum bulk ESS) because a
single stalled chain in either mode moves the minimum; the model-level numbers
are noise. Fast mode does not change how many gradients the sampler needs.

## Reproduce

```
python3 harnesses/fast_sampling_check.py select deps/posteriordb \
    --references <posteriordb checkout with reference_posteriors> \
    --census census.json --verify-log verify_fast.tsv --out models.json
python3 harnesses/fast_sampling_check.py run deps/posteriordb --models models.json --out runs/main
python3 harnesses/fast_sampling_check.py analyze deps/posteriordb \
    --references <same checkout> --models models.json --out runs/main
python3 harnesses/fast_sampling_check.py report --out runs/main --models models.json --csv main.csv
python3 harnesses/fast_sampling_check.py persist runs/main runs/seed1 runs/seed2
python3 harnesses/fast_sampling_check.py control runs/seed1 runs/seed2 --only model_a,model_b
```

`census.json` is the output of `harnesses/collapse_census.py --out`;
`verify_fast.tsv` is the per-model table of `tools/verify_refs.py --fast-math
--per-model`. The draws are not kept (2.9 GB gzipped for the first run); the
summaries needed to redo every table are the CSV and markdown files in the
data directory.

## Not covered

- Only the 120 models above. A model outside them has no sampling evidence
  for fast mode.
- One run per model for the 103 unflagged models, so a difference smaller than
  a few MCSE in a single quantity would not show.
- Apple Silicon only. x86-64 was not sampled.
- The thresholds treat the 4,000 draws as one sample, which the control above
  shows is too tight for the sampler statistics (rates, R-hat). A stricter
  screen would put between-seed variance in the denominator and needs about
  five seeds per arm per model.
