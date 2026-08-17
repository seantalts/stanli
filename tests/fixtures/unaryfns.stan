// Ten one-argument functions the conformance sweep enumerates over every
// container shape: eight from Stan's scalar math library plus the named
// spellings of unary minus and plus. Each needs a graph lowering entry AND
// an MIR interpreter branch. The transformed data block below runs the
// interpreter on doubles and the model block runs the graph kernels, so a
// name taught to one and not the other is caught here rather than surfacing
// as a wrong lp with no exception -- which is how vectorized log_sum_exp
// went wrong.
data {
  matrix[2, 3] mat;
}
transformed data {
  vector[3] td_gamma = tgamma([0.5, 1.5, 2.5]');
  vector[3] td_tri = trigamma([0.5, 1.5, 2.5]');
  real td_qf = std_normal_qf(0.6);
  real td_lqf = std_normal_log_qf(-0.5);
  real td_w0 = lambert_w0(0.5);
  real td_wm1 = lambert_wm1(-0.2);
  real td_erfc = inv_erfc(0.75);
  real td_phi = Phi_approx(0.25);
  vector[3] td_minus = minus([1.5, 2.5, 3.5]');
  vector[3] td_plus = plus([1.5, 2.5, 3.5]');
  real td_sum = sum(td_gamma) + sum(td_tri) + td_qf + td_lqf + td_w0 + td_wm1
                + td_erfc + td_phi + sum(td_minus) + sum(td_plus);
}
parameters {
  vector[3] a;
}
model {
  // The restricted domains, built out of the parameters so that nothing
  // folds away: u in (0, 1), lu <= 0, wm in (-0.2, 0) which is inside
  // lambert_wm1's branch, and pos > 0.
  vector[3] u = inv_logit(a);
  vector[3] lu = log(u);
  vector[3] wm = -0.2 * u;
  vector[3] pos = exp(a);
  target += sum(tgamma(pos));
  target += sum(trigamma(pos));
  target += sum(lambert_w0(pos));
  target += sum(lambert_wm1(wm));
  target += sum(std_normal_qf(u));
  target += sum(std_normal_log_qf(lu));
  target += sum(inv_erfc(u));
  target += sum(Phi_approx(a));
  target += sum(minus(a));
  target += sum(plus(a));
  // A matrix leaf: the declaration only accepts a 2x3 logical view, so a
  // result that kept the width but lost the extents dies here.
  matrix[2, 3] mm = Phi_approx(mat + a[1]);
  target += sum(mm);
  // A nested array, the shape the sweep enumerates to eight deep.
  array[2] vector[3] na = tgamma({pos, u});
  target += sum(na[1]) + sum(na[2]);
  target += td_sum;
}
