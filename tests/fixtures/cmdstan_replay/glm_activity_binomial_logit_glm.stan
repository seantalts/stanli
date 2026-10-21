data {
  int N; int K; int C;
  array[N] int<lower=0> y;
  array[N] int<lower=0> Nt;
  matrix[N,K] x_d;
  real alpha_d;
  vector[K] beta_d;
}
parameters {
  real dummy;
  matrix[N,K] x_p;
  real alpha_p;
  vector[K] beta_p;
}
model {
  dummy ~ normal(0,1);

  alpha_p ~ normal(0, 1);
  beta_p ~ normal(0, 1);
  y ~ binomial_logit_glm(Nt, x_d, alpha_d, beta_d);
  y ~ binomial_logit_glm(Nt, x_d, alpha_d, beta_p);
  y ~ binomial_logit_glm(Nt, x_d, alpha_p, beta_d);
  y ~ binomial_logit_glm(Nt, x_d, alpha_p, beta_p);
  y ~ binomial_logit_glm(Nt, x_p, alpha_d, beta_d);
  y ~ binomial_logit_glm(Nt, x_p, alpha_d, beta_p);
  y ~ binomial_logit_glm(Nt, x_p, alpha_p, beta_d);
  y ~ binomial_logit_glm(Nt, x_p, alpha_p, beta_p);
}