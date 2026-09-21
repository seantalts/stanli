# lp__ off by a constant: compact densities and the lite build

Most of stanli's densities reproduce CmdStan's `lp__` to the bit. A long
tail of densities does not, and an optional build extends that trade to the
whole library. In both cases every gradient stays bitwise. This page says
what the offset is, which densities carry it, and how to check.

## What the offset is

stan-math decides which terms of a log density to keep by looking at the
argument types. `y ~ normal(mu, sigma)` with `sigma` as data drops
`-log(sigma)`; the same statement with `sigma` as a parameter keeps it.
Reproducing that exactly costs one template instantiation per activity mask
(which arguments are autodiff), twice over for propto, and again for the
elementwise form: `4 * 2^N` copies of the template per distribution, about
630 KB of object each for a three-argument density.

A compact density instantiates the template once, with every argument bound
as autodiff. Everything is active, so stan-math keeps terms CmdStan drops,
and `lp__` comes out higher by exactly those terms. They are constants in the
parameters, which is the only reason CmdStan could drop them, so every partial
derivative is identical. Measured: `multi_student_t` shifts lp by
+0.81998355873446 with zero gradient difference; `lkj_cov` by
+0.28768207245200, also zero.

| | exact density | compact density or lite build |
|---|---|---|
| gradients vs CmdStan | bitwise | bitwise |
| `write_array` values | bitwise | bitwise |
| `lp__` vs CmdStan | bitwise | a per-model constant higher |
| draws for a pinned seed | same | a different chain from the same posterior |

The offset is invisible where people watch `lp__`, which is convergence
checking. It matters when an application needs the absolute log density, or
compares `lp__` across engines.

## Which densities are compact

Exact, with activity-mask dispatch: the 13 distributions models lean on
(`normal`, `cauchy`, `student_t`, `gamma`, `beta`, `lognormal`, `uniform`,
`double_exponential`, `exponential`, `inv_gamma`, `std_normal`, `weibull`,
`logistic`), the discrete ones with an integer outcome, `multi_normal` and
its cholesky form, `lkj_corr_cholesky`, `dirichlet`, and the GLMs. These are
what corpus models use, and `tools/verify_refs.py` holds them to bitwise
`lp__` on every push.

Compact: the multivariate and multinomial tail (`multi_normal_prec`,
`multi_student_t` and its cholesky form, the wishart family, `multi_gp` and
its cholesky form, `lkj_cov`, the multinomial family, `ordered_probit`,
`wiener`). In the source, compact kernels live in
`runtime/kernels/matrix_fns.cpp` on a nested var tape rather than in
`densities.cpp` behind `mask_dispatch`; for scalar densities the fourth field
of `STANLI_SCALAR_DENSITY_LIST` in `optable.hpp` encodes the tier.

Full fidelity for the tail would cost more than the rest of the library:
giving just the 13 common distributions' cdfs the mask dispatch measured
4.8 MB, against 2.2 MB for all 72 distribution functions without it. To make
a tail density exact, give it the mask dispatch; `multi_normal` in
`matrix_fns.cpp` shows the shape, an if-chain over the mask bits binding each
argument as `var` or `double`. That is `2^N` branches and `2^N`
instantiations, and it is why `multi_normal` is exact while
`multi_normal_prec`, the same kernel with one call swapped, is not.

## The lite build

`-DSTANLI_LITE_LP=ON` makes every density compact by clearing the propto
half of the instantiation ladder (`density_tier()` in
`runtime/include/stanli/optable.hpp`). The library is 48% smaller (15.75 MB
to 8.43 MB stripped on macOS arm64) and roughly halves the wasm payload.
Measured over the 119-model posteriordb corpus, every gradient and every
`write_array` value is bitwise identical to the exact build, and `lp__`
differs by a constant at every evaluation point. Speed is unchanged: the
propto choice is internal to a kernel and never changes the graph.

It is off by default in every build, browser included, so any run's `lp__`
can be compared with CmdStan directly. `stanli_exact_lp()` (Python
`stanli.exact_lp()`, JS `fit.exactLp`) reports which build is loaded. Turn it
on when download size matters more than a CmdStan-comparable `lp__`: an
embedded target or a size-critical web deployment.

Draws under a pinned seed differ between the builds. In exact arithmetic a
constant lp shift cancels in every Hamiltonian difference NUTS looks at; in
floating point, `H = -lp + kinetic` rounds differently once `lp` is shifted.
The difference starts at one ULP and grows because NUTS is chaotic: on eight
schools at the same seed, `mu` differs by 2.0e-15 after 5 warmup iterations,
6.3e-13 after 20, and 1.3e-09 after 50. This is the same class of difference
as changing the seed. A run that must reproduce another byte for byte needs
both to be the same build.

## How to check

`tools/verify_lite.py` replays both builds over the corpus and enforces the
two promises: gradients bitwise, and `lp_exact - lp_lite` the same number at
all three points of every model.

```
cmake -B build      -DCMAKE_BUILD_TYPE=Release
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
build_jobs=$(tools/build_jobs.sh)
cmake --build build      --parallel "$build_jobs" --target stanli_check
cmake --build build-lite --parallel "$build_jobs" --target stanli_check
python3 tools/verify_lite.py deps/posteriordb
```

The gate on the shift is relative to `lp` rather than to the shift itself: dropping
a term reassociates the sum after it, so the residue lives on `lp`'s rounding
scale (half an ULP of `lp` on the worst model). The lite build is not in CI
because covering it means a second full stan-math compile.

For one compact density in the exact build, there is no automated check,
because the corpus does not use those densities. The manual version, for a
model that uses the density:

```
build/stanli_check model.stan data.json --point 0   # and 1, 2
```

against `tools/ref_driver.cpp` compiled with CmdStan on the same model.
Gradients must agree to the bit, and the `lp__` difference must be the same
number at all three points. Anything else is a bug.
