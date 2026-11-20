// Vectorized forms of the scalar RNG families, and the library RNGs with
// rectangular and non-symmetric matrix arguments and parameter-dependent
// inputs. CmdStan seeds the same stream, so every draw compares exactly.
transformed data {
  matrix[2, 4] omega = [[-0.7, -1.2, -0.3, -2.0], [-1.1, -0.4, -0.9, -0.1]];
  matrix[2, 2] Gamma = [[0.9, 0.1], [0.2, 0.8]];
  matrix[4, 2] X = [[0.5, -0.3], [1.0, 0.2], [-0.4, 0.8], [0.1, 0.1]];
  matrix[3, 3] S = [[2, 0.3, 0.1], [0.3, 1, 0.2], [0.1, 0.2, 1.5]];
  matrix[3, 3] L = cholesky_decompose(S);
  vector[3] alpha = [1.5, 2.0, 0.7]';
}
parameters {
  real mu;
  real<lower=0> tau;
}
model {
  mu ~ normal(0, 1);
  tau ~ lognormal(0, 1);
}
generated quantities {
  vector[3] th = softmax([mu, 0.3, -0.2]');
  array[3] int mn = multinomial_rng(th, 12);
  array[3] int ml = multinomial_logit_rng([mu, 0.3, -0.2]', 7);
  array[3] int dm = dirichlet_multinomial_rng(alpha * tau, 9);
  array[4] int hl = hmm_latent_rng(omega, Gamma, [0.6, 0.4]');
  vector[3] st = multi_student_t_rng(4 + tau, [mu, 0, 1]', S);
  vector[3] str = multi_student_t_rng(3.0, [mu, 0, 1], S);
  vector[3] stc = multi_student_t_cholesky_rng(5.0, [mu, 0, 1]', L);
  int ol = ordered_logistic_rng(mu, [-0.5, 0.2, 1.5]');
  int op = ordered_probit_rng(mu, [-0.5, 1.5]');
  int hg = hypergeometric_rng(5, 8, 6);
  matrix[4, 4] lk = lkj_corr_rng(4, tau);
  matrix[4, 4] lkc = lkj_corr_cholesky_rng(4, tau);
  matrix[3, 3] w = wishart_rng(5.0, S);
  matrix[3, 3] wc = wishart_cholesky_rng(5.0, L);
  matrix[3, 3] iw = inv_wishart_rng(5.0, S);
  matrix[3, 3] iwc = inv_wishart_cholesky_rng(5.0, L);
  array[4] int bg = bernoulli_logit_glm_rng(X, [mu, 0, 0.1, -0.1]',
                                            [0.7, -0.5]');
  array[3] int bnb = beta_neg_binomial_rng(alpha + 1, 3.0, alpha);
  array[3] real bp = beta_proportion_rng(th, 5.0);
  array[2] int dr = discrete_range_rng({1, 2}, {4, 9});
  array[3] real emn = exp_mod_normal_rng(mu, alpha, 2.0);
  array[3] real fr = frechet_rng(alpha, tau);
  array[3] real ics = inv_chi_square_rng(alpha);
  array[3] real ll = loglogistic_rng(alpha, 3.0);
  array[3] int nb = neg_binomial_rng(alpha, 0.5);
  array[3] real pa = pareto_rng(1.5, alpha);
  array[3] real pt2 = pareto_type_2_rng(mu, alpha, 3.0);
  array[3] real ra = rayleigh_rng(alpha);
  array[3] real sics = scaled_inv_chi_square_rng(3.0, alpha);
  array[3] real sde = skew_double_exponential_rng(mu, alpha, 0.3);
  array[3] real sn = skew_normal_rng(mu, 1.3, alpha);
  array[3] real vm = von_mises_rng(mu, alpha);
}
