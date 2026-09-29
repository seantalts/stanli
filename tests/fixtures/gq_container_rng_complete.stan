parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  vector[3] a = [exp(x), 1.5, 2.3]';
  array[3] real b = {0.8, 1.1, 1.4};
  row_vector[3] c = [1.2, 1.6, 2.0];
  array[3] int trials = {3, 5, 7};
  real before = normal_rng(0, 1);
  array[22,3] real direct;
  array[22,3] real region;
  int i = 0;
  direct[1] = poisson_log_rng(a);
  direct[2] = uniform_rng(a, a + 3);
  direct[3] = bernoulli_rng(inv_logit(a));
  direct[4] = normal_rng(a, b);
  direct[5] = lognormal_rng(a, 0.7);
  direct[6] = binomial_rng(trials, inv_logit(a));
  direct[7] = gumbel_rng(0.2, a);
  direct[8] = beta_binomial_rng(trials, a, b);
  direct[9] = exponential_rng(a);
  direct[10] = poisson_rng(a);
  direct[11] = student_t_rng(a, b, c);
  direct[12] = bernoulli_logit_rng(a);
  direct[13] = gamma_rng(a, b);
  direct[14] = inv_gamma_rng(2.1, a);
  direct[15] = beta_rng(a, 1.7);
  direct[16] = chi_square_rng(a);
  direct[17] = cauchy_rng(a, b);
  direct[18] = double_exponential_rng(a, b);
  direct[19] = logistic_rng(a, b);
  direct[20] = weibull_rng(a, b);
  direct[21] = neg_binomial_2_rng(a, b);
  direct[22] = neg_binomial_2_log_rng(a, b);
  while (i < 1) {
    region[1] = poisson_log_rng(a);
    region[2] = uniform_rng(a, a + 3);
    region[3] = bernoulli_rng(inv_logit(a));
    region[4] = normal_rng(a, b);
    region[5] = lognormal_rng(a, 0.7);
    region[6] = binomial_rng(trials, inv_logit(a));
    region[7] = gumbel_rng(0.2, a);
    region[8] = beta_binomial_rng(trials, a, b);
    region[9] = exponential_rng(a);
    region[10] = poisson_rng(a);
    region[11] = student_t_rng(a, b, c);
    region[12] = bernoulli_logit_rng(a);
    region[13] = gamma_rng(a, b);
    region[14] = inv_gamma_rng(2.1, a);
    region[15] = beta_rng(a, 1.7);
    region[16] = chi_square_rng(a);
    region[17] = cauchy_rng(a, b);
    region[18] = double_exponential_rng(a, b);
    region[19] = logistic_rng(a, b);
    region[20] = weibull_rng(a, b);
    region[21] = neg_binomial_2_rng(a, b);
    region[22] = neg_binomial_2_log_rng(a, b);
    i += 1;
  }
  real after = normal_rng(0, 1);
}
