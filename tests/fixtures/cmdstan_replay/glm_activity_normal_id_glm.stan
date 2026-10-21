data {
  int N; int K; int C;
  vector[N] y;
  matrix[N,K] x_d;
  real alpha_d;
  vector[K] beta_d;
  real<lower=0> sigma_d;
}
parameters {
  real dummy;
  matrix[N,K] x_p;
  real alpha_p;
  vector[K] beta_p;
  real<lower=0> sigma_p;
}
model {
  dummy ~ normal(0,1);

  alpha_p ~ normal(0, 1);
  beta_p ~ normal(0, 1);

  y ~ normal_id_glm(x_d, alpha_d, beta_d, sigma_d);
  y ~ normal_id_glm(x_d, alpha_d, beta_d, sigma_p);
  y ~ normal_id_glm(x_d, alpha_d, beta_p, sigma_d);
  y ~ normal_id_glm(x_d, alpha_d, beta_p, sigma_p);
  y ~ normal_id_glm(x_d, alpha_p, beta_d, sigma_d);
  y ~ normal_id_glm(x_d, alpha_p, beta_d, sigma_p);
  y ~ normal_id_glm(x_d, alpha_p, beta_p, sigma_d);
  y ~ normal_id_glm(x_d, alpha_p, beta_p, sigma_p);
  y ~ normal_id_glm(x_p, alpha_d, beta_d, sigma_d);
  y ~ normal_id_glm(x_p, alpha_d, beta_d, sigma_p);
  y ~ normal_id_glm(x_p, alpha_d, beta_p, sigma_d);
  y ~ normal_id_glm(x_p, alpha_d, beta_p, sigma_p);
  y ~ normal_id_glm(x_p, alpha_p, beta_d, sigma_d);
  y ~ normal_id_glm(x_p, alpha_p, beta_d, sigma_p);
  y ~ normal_id_glm(x_p, alpha_p, beta_p, sigma_d);
  y ~ normal_id_glm(x_p, alpha_p, beta_p, sigma_p);
}