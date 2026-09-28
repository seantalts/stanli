parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  vector[11] draws;
  real a = exp(x) + 1;
  real b = 1.5;
  int i = 0;
  while (i < 1) {
    draws[1] = std_normal_rng();
    draws[2] = gamma_rng(a, b);
    draws[3] = inv_gamma_rng(a, b);
    draws[4] = beta_rng(a, b);
    draws[5] = chi_square_rng(a);
    draws[6] = cauchy_rng(a, b);
    draws[7] = double_exponential_rng(a, b);
    draws[8] = logistic_rng(a, b);
    draws[9] = weibull_rng(a, b);
    draws[10] = neg_binomial_2_rng(a, b);
    draws[11] = neg_binomial_2_log_rng(a, b);
    i += 1;
  }
  real after = normal_rng(x, 1);
}
