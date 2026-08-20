// The five distribution functions the recorder cannot evaluate at all.
// von_mises's three write `res *= 0.0` and compare `x_n == -pi` on the
// scalar type, and neg_binomial_2's lcdf and lccdf form
// `phi / (phi + mu)`; none of that goes through stan-math's partials
// propagator, and `rvar` has no operators, so listing them beside the
// generated cdfs is a compile error rather than a wrong answer. They get
// the nested var tape instead, in matrix_fns.cpp.
//
// Both the vectorized and the scalar form of every one, because the
// kernel binds a length-1 slot as a scalar rather than a one-element
// vector: stan-math's sequence views broadcast a scalar but require a
// vector to match the other arguments' size, so getting that wrong is
// every lane evaluated with element 0's parameters rather than an error.
//
// No parameter feeds two of these calls. Each density evaluates on its
// own nested tape and adds its adjoints into the slot, so a shared
// parameter would be summed op by op here and in one reverse sweep in
// the var reference -- a reassociation of a few ULP that would cost the
// test its bitwise comparison for nothing.
data {
  int n;
  array[3] int ns;
}
parameters {
  vector[3] y1;
  vector[3] k1;
  vector[3] y2;
  vector[3] k2;
  real y3;
  real k3;
  vector[3] m4;
  vector[3] p4;
  real m5;
  vector[3] m6;
}
model {
  target += von_mises_cdf(y1 | 0.1, exp(k1));
  target += von_mises_lcdf(y2 | [0.1, 0.2, 0.3]', exp(k2));
  target += von_mises_lccdf(y3 | 0.1, exp(k3));
  target += neg_binomial_2_lcdf(ns | exp(m4), exp(p4));
  target += neg_binomial_2_lccdf(n | exp(m5), 2.0);
  // A language-level scalar outcome against vectorized reals:
  // the lowering replicates it to the lane count, because the
  // kernel maps the whole integer group as one vector and a
  // size-1 container loses to a longer argument on
  // check_consistent_sizes.
  target += neg_binomial_2_lccdf(n | exp(m6), 2.0);
}
