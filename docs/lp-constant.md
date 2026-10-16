# lp__ off by a constant: compact densities and the lite build

Most of stanli's densities reproduce CmdStan's `lp__` to the bit. A tail of
less common densities reports `lp__` shifted by a constant, and an optional
build extends that trade to the whole library. In both cases every gradient
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
derivative is identical. For example, `multi_student_t` shifts lp by
+0.81998355873446 and `lkj_cov` by +0.28768207245200, both with zero gradient
difference.

| | exact density | compact density or lite build |
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

These are the densities corpus models use, and
[`tools/verify_refs.py`](../tools/verify_refs.py) checks their `lp__` against
CmdStan on every source pull request.

Compact: the multivariate and multinomial tail (`multi_normal_prec`,
`multi_student_t` and its Cholesky form, the Wishart family, `multi_gp` and
its Cholesky form, `lkj_cov`, the multinomial family, `ordered_probit`,
`wiener`). These kernels live in
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

## The lite build

`-DSTANLI_LITE_LP=ON` makes every density compact by clearing the propto bit
of each tier (`density_tier()` in
[`optable.hpp`](../runtime/include/stanli/optable.hpp)). The library is 48%
smaller (15.75 MB to 8.43 MB stripped on macOS arm64), which roughly halves
the wasm payload. Over the 119-model posteriordb corpus, every gradient and
every `write_array` value is bitwise identical to the exact build, and `lp__`
differs by a constant at every evaluation point. Speed is unchanged, because
the propto choice is internal to a kernel and never changes the graph.

The lite build is off in every shipped build, browser included, so any run's
`lp__` can be compared with CmdStan directly. `stanli_exact_lp()` (Python
`stanli.exact_lp()`, JavaScript `fit.exactLp`) reports which build is loaded.
Turn it on when download size matters more than a CmdStan-comparable `lp__`,
for example on an embedded target or a size-critical web deployment.

Draws under a pinned seed differ between the builds. In exact arithmetic a
constant shift cancels in every Hamiltonian difference NUTS looks at. In
floating point, `H = -lp + kinetic` rounds differently once `lp` is shifted.
The difference starts at one ULP and grows because NUTS is chaotic: on eight
schools at the same seed, `mu` differs by 2.0e-15 after 5 warmup iterations,
6.3e-13 after 20, and 1.3e-09 after 50. This is the same kind of difference as
changing the seed. A run that must reproduce another byte for byte needs both
to use the same build.

## How to check

`tools/verify_lite.py` replays both builds over the corpus and enforces both
promises: gradients bitwise, and `lp_exact - lp_lite` the same number at all
three points of every model.

```sh
cmake -B build      -DCMAKE_BUILD_TYPE=Release
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
build_jobs=$(tools/build_jobs.sh)
cmake --build build      --parallel "$build_jobs" --target stanli_check
cmake --build build-lite --parallel "$build_jobs" --target stanli_check
python3 tools/verify_lite.py deps/posteriordb
```

The gate on the shift is relative to `lp`. Dropping a term reassociates the
sum after it, so the residue sits on `lp`'s rounding scale (half an ULP of
`lp` on the worst model). CI does not build the lite configuration, because
that needs a second full stan-math compile.

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
