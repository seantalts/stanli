# lp__ and dropped constants

Under `~`, and in `target += foo_lupdf(...)`, Stan Math leaves out the terms
of a log density that depend only on data. They do not change the posterior.
stanli leaves out the same terms, so its `lp__` matches CmdStan's.

Stan Math decides which terms to keep from the argument types.
`y ~ normal(mu, sigma)` with `sigma` as data drops `-log(sigma)`; with
`sigma` as a parameter it keeps it. A stanli kernel therefore passes each data
argument to Stan Math as `double` and each parameter as an autodiff variable.
Every combination costs one template instantiation, so a kernel binds only the
arguments whose type changes which terms survive. The kernels are in
[`runtime/kernels/densities.cpp`](../runtime/kernels/densities.cpp) and
[`runtime/kernels/matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp).

`multi_student_t` and its Cholesky form bind every argument as autodiff and
then subtract the `nu` and `Sigma` terms Stan Math drops when those arguments
are data. Their `lp__` agrees with CmdStan to rounding rather than to the bit.

## Releases up to 0.26.0

These densities bound every argument as autodiff to save binary size, kept
terms CmdStan drops, and reported `lp__` shifted by a constant:

- `wishart`, `inv_wishart` and their Cholesky forms, `multi_gp` and its
  Cholesky form, and `lkj_cov`, whenever an argument was data;
- `lkj_corr`, `lkj_corr_cholesky`, the multinomial family and `wiener`, only
  when every argument was data.

The shift did not depend on the parameters, so gradients, `write_array` values
and the posterior were unchanged. It was lower than CmdStan's for the Wishart
family: `S ~ inv_wishart(4, I)` on a 2 x 2 matrix reported 3.224 less. A
pinned seed gave a different chain from the same posterior. Binding the data
arguments as `double` made the shared library 208 KB larger.

## How it is checked

[`tests/fixtures/propto_data_arguments.stan`](../tests/fixtures/propto_data_arguments.stan)
and [`propto_multi_gp.stan`](../tests/fixtures/propto_multi_gp.stan) call
each of the densities above under `~` with every split of its arguments into
data and parameters, against CmdStan references. The corpus replay checks the
densities corpus models use. The generated signature models do not: they call
`foo_lpdf(...)`, which always keeps every term.
