// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  vector[2] theta = [0.3, 0.7]';
  vector[2] lam = [-1.5, -0.25]';
  array[2] vector[2] lams = {[-1.5, -0.25]', [0.5, -2]'};
  matrix[2, 2] Gamma = [[0.9, 0.1], [0.2, 0.8]];
  matrix[2, 1] log_omega = [[log(0.5)], [log(0.25)]];
  vector[2] rho = [0.6, 0.4]';
  if (abs(inc_beta(1, 1, 0.3) - 0.3) > 1e-12) reject("inc_beta");
  if (abs(inc_beta(2.5, 1.5, inv_inc_beta(2.5, 1.5, 0.4)) - 0.4) > 1e-10)
    reject("inv_inc_beta");
  if (abs(hypergeometric_2F1(1.5, 2, 2, 0.25) - pow(0.75, -1.5)) > 1e-10)
    reject("hypergeometric_2F1");
  if (abs(hypergeometric_3F2({1.0, 2.0, 1.5}, {2.5, 3.0}, 0.4)
          - hypergeometric_pFq({1.0, 2.0, 1.5}, {2.5, 3.0}, 0.4)) > 1e-12)
    reject("hypergeometric_3F2");
  if (abs(hypergeometric_pFq([1, 2]', [2]', 0.5) - 2) > 1e-12)
    reject("hypergeometric_pFq");
  if (int_step(2) != 1 || int_step(0) != 0 || int_step(-3) != 0
      || int_step(0.5) != 1 || int_step(-0.5) != 0)
    reject("int_step");
  if (abs(log_mix(theta, lam) - log_sum_exp(log(theta) + lam)) > 1e-12)
    reject("log_mix");
  if (abs(log_mix(theta, lams)
          - (log_sum_exp(log(theta) + lams[1])
             + log_sum_exp(log(theta) + lams[2]))) > 1e-12)
    reject("log_mix array");
  if (abs(log_mix(0.3, -1.5, -0.25) - log_mix(theta, lam)) > 1e-12)
    reject("log_mix scalar");
  if (abs(discrete_range_cdf(3 | 1, 5) - 0.6) > 1e-12) reject("drange cdf");
  if (abs(discrete_range_lcdf(3 | 1, 5) - log(0.6)) > 1e-12) reject("drange lcdf");
  if (abs(discrete_range_lccdf(3 | 1, 5) - log(0.4)) > 1e-12)
    reject("drange lccdf");
  if (abs(quantile({1.0, 2.0, 3.0, 4.0, 5.0}, 0.5) - 3) > 1e-12)
    reject("quantile");
  if (abs(hmm_marginal(log_omega, Gamma, rho) - log(0.6 * 0.5 + 0.4 * 0.25))
      > 1e-12)
    reject("hmm_marginal");
}
parameters {
  real<lower=0, upper=1> x;
  real<lower=0.1> a;
  vector<lower=0, upper=1>[2] th;
  vector[2] lm;
  array[2] vector[2] lms;
}
model {
  if (abs(inc_beta(1, 1, x) - x) > 1e-9) reject("inc_beta param");
  if (abs(inc_beta(a, 2, inv_inc_beta(a, 2, x)) - x) > 1e-7)
    reject("inv_inc_beta param");
  if (abs(hypergeometric_2F1(a, 2, 2, x) - pow(1 - x, -a)) > 1e-8)
    reject("hypergeometric_2F1 param");
  if (abs(hypergeometric_3F2({a, 2.0, 1.5}, {2.5, 3.0}, x)
          - hypergeometric_pFq({a, 2.0, 1.5}, {2.5, 3.0}, x)) > 1e-9)
    reject("hypergeometric_3F2 param");
  if (int_step(a - 0.1) != 1 || int_step(-a) != 0) reject("int_step param");
  vector[2] simplex_th = th / sum(th);
  if (abs(log_mix(simplex_th, lm) - log_sum_exp(log(simplex_th) + lm)) > 1e-9)
    reject("log_mix param");
  if (abs(log_mix(simplex_th, lms)
          - (log_sum_exp(log(simplex_th) + lms[1])
             + log_sum_exp(log(simplex_th) + lms[2]))) > 1e-9)
    reject("log_mix array param");
  target += -0.5 * square(a) - 0.5 * dot_self(lm);
}
