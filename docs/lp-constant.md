# lp__ off by a constant: compact densities

Most of stanli's densities reproduce CmdStan's `lp__` to the bit. A tail of
less common densities reports `lp__` shifted by a constant. Every gradient
is bitwise identical to CmdStan's. This page explains the shift, lists which
densities carry it, and shows how to check.

## What the shift is

stan-math decides which terms of a log density to keep by looking at the
argument types. `y ~ normal(mu, sigma)` with `sigma` as data drops
`-log(sigma)`; the same statement with `sigma` as a parameter keeps it.
Reproducing that exactly costs one template instantiation per activity mask
(which arguments are autodiff), twice for propto, and again for the
elementwise form: `4 * 2^N` copies per distribution, about 630 KB of object
for a three-argument density.

A compact density instantiates the template once, with every argument bound
as autodiff. Everything is active, so stan-math keeps terms CmdStan drops, and
`lp__` comes out higher by exactly those terms. They are constant in the
parameters, which is the only reason CmdStan could drop them, so every partial
derivative is identical. For example, `lkj_cov` shifts lp by
+0.28768207245200 with zero gradient difference.

| | exact density | compact density |
| --- | --- | --- |
| gradients vs CmdStan | bitwise | bitwise |
| `write_array` values | bitwise | bitwise |
| `lp__` vs CmdStan | bitwise | a per-model constant higher |
| draws for a pinned seed | same | a different chain from the same posterior |

Convergence checks on `lp__` cannot see the shift. It matters when an
application needs the absolute log density or compares `lp__` across engines.

## Which densities are compact

Exact `lp__`, through activity-mask dispatch:

- every scalar density in `STANLI_SCALAR_DENSITY_LIST`
  ([`optable.hpp`](../runtime/include/stanli/optable.hpp)), including the 13
  that models use most (`normal`, `cauchy`, `student_t`, `gamma`, `beta`,
  `lognormal`, `uniform`, `double_exponential`, `exponential`, `inv_gamma`,
  `std_normal`, `weibull`, `logistic`). The list's last field is the
  instantiation tier; its propto bit is what keeps `lp__` exact.
- the fused `normal`, `cauchy`, `student_t`, `lognormal`, `beta`, `gamma` and
  `ordered_logistic` kernels, which are tested bitwise against Stan Math.
- the discrete densities with an integer outcome, `multi_normal` and its
  Cholesky form, `lkj_corr_cholesky`, `dirichlet`, and the GLMs.
- `multi_student_t` and its Cholesky form. Their kernel binds every argument
  as var and then subtracts the `nu` and `Sigma` terms Stan Math drops when
  those arguments are data, so `lp__` agrees with CmdStan to rounding rather
  than to the bit.

These are the densities corpus models use, and
[`tools/verify_refs.py`](../tools/verify_refs.py) checks their `lp__` against
CmdStan on every source pull request.

Compact: the multivariate and multinomial tail (`multi_normal_prec`, the
Wishart family, `multi_gp` and its Cholesky form, `lkj_cov`, the multinomial
family, `ordered_probit`, `wiener`). These kernels live in
[`runtime/kernels/matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp) on a
nested var tape.

Full fidelity for the tail would cost more than the rest of the library:
giving only the 13 common distributions' CDFs the mask dispatch measured
4.8 MB, against 2.2 MB for all 72 distribution functions without it. To make a
tail density exact, give it the mask dispatch. `multi_normal` in
`matrix_fns.cpp` shows the shape: an if-chain over the mask bits that binds
each argument as `var` or `double`. That is `2^N` branches and `2^N`
instantiations, and it is why `multi_normal` is exact while
`multi_normal_prec`, the same kernel with one call swapped, is compact.

## How to check

No automated check covers a compact density in the exact build, because the
corpus does not use those densities. To check one by hand, run a model that
uses it at each of the three points:

```sh
build/stanli_check model.stan data.json --point 0   # and 1, 2
```

and compare with [`tools/ref_driver.cpp`](../tools/ref_driver.cpp) compiled
with CmdStan on the same model. Gradients must agree to the bit, and the
`lp__` difference must be the same number at all three points. Anything else
is a bug.
