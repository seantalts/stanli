data {
  int N; int K; int C;
  array[N] int<lower=0> y;
  matrix[N,K] x_d;
  real alpha_d;
  vector[K] beta_d;
  real<lower=0> phi_d;
}
parameters {
  real dummy;
  matrix[N,K] x_p;
  real alpha_p;
  vector[K] beta_p;
  real<lower=0> phi_p;
}
model {
  dummy ~ normal(0,1);

  alpha_p ~ normal(0, 1);
  beta_p ~ normal(0, 1);

  y ~ neg_binomial_2_log_glm(x_d, alpha_d, beta_d, phi_d);
  y ~ neg_binomial_2_log_glm(x_d, alpha_d, beta_d, phi_p);
  y ~ neg_binomial_2_log_glm(x_d, alpha_d, beta_p, phi_d);
  y ~ neg_binomial_2_log_glm(x_d, alpha_d, beta_p, phi_p);
  y ~ neg_binomial_2_log_glm(x_d, alpha_p, beta_d, phi_d);
  y ~ neg_binomial_2_log_glm(x_d, alpha_p, beta_d, phi_p);
  y ~ neg_binomial_2_log_glm(x_d, alpha_p, beta_p, phi_d);
  y ~ neg_binomial_2_log_glm(x_d, alpha_p, beta_p, phi_p);
  y ~ neg_binomial_2_log_glm(x_p, alpha_d, beta_d, phi_d);
  y ~ neg_binomial_2_log_glm(x_p, alpha_d, beta_d, phi_p);
  y ~ neg_binomial_2_log_glm(x_p, alpha_d, beta_p, phi_d);
  y ~ neg_binomial_2_log_glm(x_p, alpha_d, beta_p, phi_p);
  y ~ neg_binomial_2_log_glm(x_p, alpha_p, beta_d, phi_d);
  y ~ neg_binomial_2_log_glm(x_p, alpha_p, beta_d, phi_p);
  y ~ neg_binomial_2_log_glm(x_p, alpha_p, beta_p, phi_d);
  y ~ neg_binomial_2_log_glm(x_p, alpha_p, beta_p, phi_p);
}