// The cumulative-distribution side of the discrete densities whose lpmf
// already worked. Three argument shapes in one model:
//
//   neg_binomial_2_cdf -- one integer group and two real arguments, the
//     layout the generated integer cdfs already had;
//   binomial -- two integer groups (outcome and trials) and one real;
//   beta_binomial -- two integer groups and two reals.
//
// Both scalar and array forms of the integer groups appear, because the
// two-group layout writes a length per group and spells a language-level
// scalar -1: an array of one is a container that must match the real
// arguments' size, a bare int broadcasts.
//
// neg_binomial_2's lcdf and lccdf are absent on purpose. Unlike the cdf
// they compute their result by arithmetic on the autodiff scalar rather
// than through stan-math's partials propagator, which the recorder
// scalar does not implement. See docs/coverage.md.
//
// theta reaches the densities directly rather than through inv_logit,
// which would put a one-ULP unary divergence between this model and its
// stan-math reference and cost the test its bitwise comparison.
data {
  int n;
  array[3] int ns;
  array[3] int N;
}
parameters {
  vector[3] theta;
}
model {
  vector[3] b = exp(theta);
  target += neg_binomial_2_cdf(ns | b, [2.0, 3.0, 4.0]');
  target += binomial_lcdf(ns | N, theta);
  target += binomial_lccdf(n | 9, theta);
  target += binomial_cdf(ns | N, theta);
  target += beta_binomial_lcdf(ns | N, b, [1.3, 1.4, 1.5]');
  target += beta_binomial_lccdf(n | 9, b, [1.3, 1.4, 1.5]');
  target += beta_binomial_cdf(ns | N, b, [1.3, 1.4, 1.5]');
}
