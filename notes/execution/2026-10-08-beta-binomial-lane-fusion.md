# Lane fusion asked `beta_binomial_lpmf` for a form it did not have

Status: bug record, 2026-10-08. Fixed in PR #443 (merge `f99f3ec7`), which
gave the kernel the missing form. Affects default mode in releases 0.9.1
through 0.19.1; the two ends were run, the releases between were not.

## What went wrong

Lane partitioning (`runtime/src/partition.cpp`) fuses a loop whose
iterations are the same computation into vector ops. When an iteration uses
a density's value instead of adding it straight to the target, the fused
density has to give one value per iteration: variant bit `0x40`, the
elementwise form. Which densities may be fused is read from the trait table
in `runtime/include/stanli/optable.hpp`, and `beta_binomial_lpmf` is listed
there with the binomials.

Its kernel (`runtime/kernels/densities_lpmf.cpp`) had only the summed form.
Asked for the elementwise one, it wrote the sum of all iterations into the
first element and left the others at zero, with partials to match. So:

- a later sum of the elements gave the right log density and a wrong
  gradient;
- anything that weighted the elements unequally gave a wrong log density as
  well.

Nothing rejected the request, and no test exercised it: the corpus models
that use `beta_binomial` add it straight to the target, which takes the
summed form.

## Which models

A loop over observations in which `beta_binomial_lpmf` is not itself the
target term. The three checked here, with 200 observations
(`data/2026-10-08-beta-binomial-lanes/`):

| model | statement in the loop | log density | gradient |
| --- | --- | --- | --- |
| `v1` | `target += x[n] * beta_binomial_lpmf(...)` | wrong (643.02 for 39.40) | wrong |
| `v3` | `lp[n] = beta_binomial_lpmf(...)`, then `target += sum(lp)` | right | wrong |
| `v5` | `target += 0.5 * beta_binomial_lpmf(...)` | right | wrong |

Not affected: `y[n] ~ beta_binomial(...)` or `target +=
beta_binomial_lpmf(...)` on its own in a loop, a vectorized statement, and
the mixtures tried (`log_mix` and `log_sum_exp` over two beta-binomial terms
per observation, and a zero-inflated form with a branch on `y[n] == 0`),
which partitioning did not fuse. Other shapes were not searched.

## Evidence

- `stanli==0.9.1` and `stanli==0.19.1` from PyPI, `Model.log_prob_grad` at
  the zero point: the values above, against the same wheel with
  `STANLI_NO_PARTITION=1` (`released_wheels.py`). With partitioning off the
  two releases agree with each other to the last digits printed.
- `main` at `614db5e1`, the commit before the fix: the same three models
  differ between passes on and `STANLI_NO_PARTITION=1 STANLI_NO_REROLL=1`.
  At `f99f3ec7` they agree to within rounding.
- `tests/test_partition.cpp`, `test_beta_binomial_elt_fusion`: a weighted
  beta-binomial lane group against the unfused graph. It fails on `614db5e1`
  (gradient entries zero, log density 28% off) and passes with the fix.

## How it was found

The fast-mode observation collapse evaluates a density once per distinct
row, also through the elementwise form. Its tests run every rewritten graph
against the graph it came from, and the beta-binomial case had the right
value and a zero gradient.

## No other density has the gap

`tests/test_partition.cpp`, `test_listed_densities_have_both_forms`, asks
every density that carries the trait for both forms at a valid point with
three elements and compares the elementwise values' sum and gradient with
the summed form (1e-12). It takes the real-argument and integer lists from
the macros that generate the kernels, adds the eight hand-listed lpmfs, and
fails if an opcode has the trait and no entry. All pass with the fix, so
`beta_binomial` was the only one.

## Left open

- 0.19.1 users are not warned anywhere but the changelog.
- Loop shapes other than the six above were not searched on the released
  wheels.
