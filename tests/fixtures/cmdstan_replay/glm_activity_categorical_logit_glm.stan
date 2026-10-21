data {
  int N; int K; int C;
  array[N] int<lower=1,upper=C> y;
  matrix[N,K] x_d;
  vector[C] alpha_d;
  matrix[K,C] beta_d;
}
parameters {
  real dummy;
  matrix[N,K] x_p;
  vector[C] alpha_p;
  matrix[K,C] beta_p;
}
model {
  dummy ~ normal(0,1);

  alpha_p ~ normal(0, 1);
  to_vector(beta_p) ~ normal(0, 1);
  y ~ categorical_logit_glm(x_d, alpha_d, beta_d);
  y ~ categorical_logit_glm(x_d, alpha_d, beta_p);
  y ~ categorical_logit_glm(x_d, alpha_p, beta_d);
  y ~ categorical_logit_glm(x_d, alpha_p, beta_p);
  y ~ categorical_logit_glm(x_p, alpha_d, beta_d);
  y ~ categorical_logit_glm(x_p, alpha_d, beta_p);
  y ~ categorical_logit_glm(x_p, alpha_p, beta_d);
  y ~ categorical_logit_glm(x_p, alpha_p, beta_p);
}