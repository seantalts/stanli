data {
  int N; int K; int C;
  array[N] int<lower=1,upper=C> y;
  matrix[N,K] x_d;
  vector[K] beta_d;
  ordered[C-1] cuts_d;
}
parameters {
  real dummy;
  matrix[N,K] x_p;
  vector[K] beta_p;
  ordered[C-1] cuts_p;
}
model {
  dummy ~ normal(0,1);

  beta_p ~ normal(0, 1);

  y ~ ordered_logistic_glm(x_d, beta_d, cuts_d);
  y ~ ordered_logistic_glm(x_d, beta_d, cuts_p);
  y ~ ordered_logistic_glm(x_d, beta_p, cuts_d);
  y ~ ordered_logistic_glm(x_d, beta_p, cuts_p);
  y ~ ordered_logistic_glm(x_p, beta_d, cuts_d);
  y ~ ordered_logistic_glm(x_p, beta_d, cuts_p);
  y ~ ordered_logistic_glm(x_p, beta_p, cuts_d);
  y ~ ordered_logistic_glm(x_p, beta_p, cuts_p);
}