// A language-level scalar int outcome against vectorized real arguments.
// The integer outcome rides in idata rather than on a propagator edge, and
// the kernels map idata as a whole vector of outcomes -- so a scalar arrived
// at stan-math as a size-1 container and every one of these threw
// "Failures variable has size = 1, but ... has size 2". stan-math broadcasts
// a real scalar without complaint; the int group had no way to say it was
// one.
//
// One line per wiring: the generated int densities, the generated int cdfs,
// a hand-written lpmf that predates both lists, and ordered_logistic --
// whose cutpoint vector is a whole argument, not lanes, so its outcome must
// NOT be broadcast to the cutpoint count.
data {
  int n;
}
parameters {
  vector[2] theta;
  real lambda;
}
model {
  vector[2] r = exp(theta);
  target += beta_neg_binomial_lpmf(n | r, [2.0, 3.0]', [1.3, 1.4]');
  target += beta_neg_binomial_lcdf(n | r, [2.0, 3.0]', [1.3, 1.4]');
  target += poisson_log_lpmf(n | theta);
  target += ordered_logistic_lpmf(2 | lambda, [-1.0, 0.5, 2.0]');
}
