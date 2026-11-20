# Programs from the Stan User's Guide

The [Stan User's Guide](https://mc-stan.org/docs/stan-users-guide/) prints
programs for features that the rest of the corpus reaches only through
language fixtures: a discrete change point summed out of the likelihood,
`integrate_1d`, the adjoint ODE solver,
`map_rect`, a copula and a capture-recapture model that is written as counts
of capture histories. This directory has six of them, each with a data file.

The guide's source is [stan-dev/docs](https://github.com/stan-dev/docs) at
commit `b07b2a99457cd04cdcded3ea9e85def099290328`. Its `LICENSE` file says
that the code is distributed under the BSD 3-clause license and the text and
images under CC BY-ND 4.0. Only code blocks are used, never the text. The
BSD part of that file, Copyright (c) 2018-2026 Stan Development Team and their
assignees, is in [`licenses/BSD-3-Clause-stan-docs`](licenses/BSD-3-Clause-stan-docs).

The guide has no `.stan` files; every program lives in a `.qmd` chapter, and
many blocks are fragments. [`tools/gen_stanguide_models.py`](../../tools/gen_stanguide_models.py)
extracts each program from a checkout of the repository at that commit, joins
blocks of one chapter in order where a program is split, and applies the edits
below, and nothing else. Rerunning it reproduces the `.stan` files byte for
byte.

| Entry | Chapter and blocks | Edit to the guide's code | What it reaches |
| --- | --- | --- | --- |
| `stanguide_changepoint` | `latent-discrete`, the change point program | none | a discrete change point summed out with `log_sum_exp`, a Poisson rate chosen by `t < s ? e : l` |
| `stanguide_cjs_history` | `latent-discrete`, the collective Cormack-Jolly-Seber program | none | capture-recapture from counts of capture histories, transformed parameters `chi` defined by recursion |
| `stanguide_map_rect_logistic` | `parallelization`, the `map_rect` logistic regression | none | `map_rect` with three shards |
| `stanguide_integrate_1d_normal` | `one-dimensional-integrals`, the `normal_density` function and the program that calls it | the function is wrapped in `functions { }` | `integrate_1d_double_exponential` with a parameter as the lower limit and an infinite upper limit |
| `stanguide_ode_adjoint` | `odes`, the harmonic oscillator fit with `ode_rk45` | the `ode_rk45` call is replaced by the guide's `ode_adjoint_tol_ctl` call from the section on the adjoint solver, with its three tolerance names written as the numbers 1e-6, 1e-6 and 1000000 | `ode_adjoint_tol_ctl` |
| `stanguide_copula` | `copulas`, the Gaussian copula with exponential marginals | the declaration `vector<lower=0>[D] y[N];` is written `array[N] vector<lower=0>[D] y;`, because stanc no longer accepts the old array syntax | `inv_Phi` of an exponential CDF, `multi_normal_cholesky` of the transformed values, `cholesky_factor_corr` with `lkj_corr_cholesky` |

The guide gives data declarations but no data. A script writes each data file:
[`tools/gen_stanguide_data.R`](../../tools/gen_stanguide_data.R) documents, in
its header, how each one is derived. `changepoint` uses the explosion dates in
the R package boot (`boot::coal`, License "Unlimited"), counted per calendar
year; the other six are simulated with `set.seed(1)`.

## Programs in the guide that are not here

- The DAE chapter's program has no `model` block: its log density is zero and
  its gradient is zero everywhere, so the replay would compare only the forward
  solve. It was recorded and dropped. The forward solve agreed with CmdStan to
  2.7e-10 relative, and at one of the three points both engines rejected the
  solve (`IDASolve ... error flag -4`). Making it a model would mean writing a
  likelihood.
- The algebraic solver chapter has a program with no data, no parameters and no
  likelihood. Making it a model would mean writing one.
- The complex number chapter's regression program has a comment where its
  error model should be, so it is not complete. The guide has no program that
  calls `fft`.
- The tuple programs of the guide are functions and `transformed data`
  printing, with no parameters.

## References

The references are CmdStan 2.40.0 values at the three shared points, recorded
as for the other imported collections (see
[`tests/examplemodels/README.md`](../examplemodels/README.md)), and replayed by
the corpus check.

## What the recording found for `stanguide_ode_adjoint`

The guide's call uses names for its tolerances (`relative_tolerance`,
`absolute_tolerance`, `max_num_steps`) that it does not define; the program
here writes them as 1e-6, 1e-6 and 1000000. At two of the three reference
points stanli and CmdStan differ by 3.9e-6 and 3.3e-6 relative in the log
density and gradient, which is more than the 1e-10 recording threshold, so the
status is `MISMATCH`. The replay holds a `MISMATCH` point to a gate that is at
least 3.3e-2, so it catches only a gross change. With every tolerance at 1e-11
the two engines agree to 5e-11 and 9e-11, so the 1e-6 difference is the noise
of the adjoint solve at that tolerance. Against the 1e-11 solution at the
first point, the 1e-6 run's log density is off by 3.0e-6 in both engines, its
first gradient entry by 4.0e-6 in stanli and 1.5e-5 in CmdStan, and its
second by 4.0e-5 in stanli and 4.4e-5 in CmdStan. The 1e-11 variant is not
in the corpus, because its 5e-11 to 9e-11 differences pass the replay but fail
the fast-mode limit of 1e-12 and would become the replay's worst-deviation
headline.
