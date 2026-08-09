# Compact densities: exact gradients, lp\_\_ off by a constant

Most of stanli's densities reproduce CmdStan's `lp__` to the bit. The
long tail does not, by choice. This is what that choice is and how to
tell which kind you are looking at.

## The claim

For a density built the compact way:

| | |
|---|---|
| gradients vs CmdStan | **bitwise** (measured, every one) |
| `write_array` values | **bitwise** |
| `lp__` vs CmdStan | a **per-model constant** higher |
| draws for a pinned seed | a different but equally valid chain |

The constant is the same at every point in parameter space, and the
verification checks exactly that: three evaluation points, same shift
each time. A shift that moved with the parameters is the only way this
can be wrong.

## Why it happens

stan-math decides which terms of a log density to keep by looking at
the argument **types**: `y ~ normal(mu, sigma)` with `sigma` as data
drops `-log(sigma)`, while the same statement with `sigma` as a
parameter keeps it. Reproducing that exactly costs one template
instantiation per activity mask (which arguments are autodiff), on top
of the full form and again for the elementwise variant: `4 * 2^N`
copies per distribution, roughly 630 KB of object for a three-argument
one.

A compact density instantiates the template **once**, with every
argument bound as autodiff. Everything is then "active", so stan-math
keeps terms CmdStan drops, and `lp__` comes out higher by exactly those
terms. They are constants (that is the only reason CmdStan could drop
them), so every partial is identical. Measured: `multi_student_t`
shifts lp by +0.81998355873446 with zero gradient difference; `lkj_cov`
by +0.28768207245200, also zero.

## Which densities are which

**Exact** (mask-dispatched): the 13 distributions models lean on
(`normal`, `cauchy`, `student_t`, `gamma`, `beta`, `lognormal`,
`uniform`, `double_exponential`, `exponential`, `inv_gamma`,
`std_normal`, `weibull`, `logistic`), the discrete ones with an integer
outcome, `multi_normal` and its cholesky form, `lkj_corr_cholesky`,
`dirichlet`, and the GLMs. These are what corpus models use, and
`tools/verify_refs.py` holds them to bitwise `lp__` on every push.

**Compact**: the multivariate and multinomial tail
(`multi_normal_prec`, `multi_student_t` and its cholesky form, the
wishart family, `multi_gp` and its cholesky form, `lkj_cov`, the
multinomial family, `ordered_probit`, `wiener`). The tier is visible in
the source: compact kernels live in `runtime/kernels/matrix_fns.cpp` on
a nested var tape rather than in `densities.cpp` behind
`mask_dispatch`; for scalar densities, the 4th field of
`STANLI_SCALAR_DENSITY_LIST` in `optable.hpp` encodes it.

## Why this is the right trade for the tail

Full fidelity for the tail would cost more than the rest of the
library: giving just the 13 common distributions' *cdfs* the mask
dispatch measured 4.8 MB, against 2.2 MB for all 72 distribution
functions without it, and the multivariate densities take matrix
arguments. And `lp__` is the one output that does not affect inference:
people watch it for convergence, where a constant offset is invisible.
The gradient is what the sampler integrates, and that is bitwise. The
same reasoning applied globally is `STANLI_LITE_LP`
([docs/lite-lp.md](lite-lp.md)), which makes every density compact and
halves the runtime.

## If you need a tail density to be exact

Give it the activity-mask dispatch. `multi_normal` in
`runtime/kernels/matrix_fns.cpp` shows the shape: an if-chain over the
mask bits binding each argument as `var` or `double`, so stan-math sees
the same types CmdStan's generated code does. It is `2^N` branches and
`2^N` instantiations, and it is why `multi_normal` is exact while
`multi_normal_prec`, the same kernel with one call swapped, is not.

## Checking it yourself

There is no automated per-density check for the shift, because the
corpus does not use these densities and `harnesses/fn_sweep.py` only
generates all-scalar models. The manual version, for a model using the
density:

```
build/stanli_check model.stan data.json --point 0   # and 1, 2
```

against `tools/ref_driver.cpp` compiled with CmdStan on the same model.
Gradients must agree to the bit; the `lp__` difference must be the same
number at all three points. Anything else is a bug, not a tier.
