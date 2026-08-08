// The GLM fast paths brms and rstanarm emit directly. These bind their
// arguments explicitly instead of through mask_dispatch, so they are the
// one density shape whose propto bit and activity mask travel a different
// route -- and the route was missing, which cost poisson_log_glm's lp
// sum(log(y!)) against CmdStan while its gradients were already exact.
data {
  int<lower=1> N;
  int<lower=1> K;
  matrix[N, K] x;
  array[N] int<lower=0> y;
}
parameters {
  real alpha;
  vector[K] beta;
  real<lower=0> phi;
}
model {
  alpha ~ normal(0, 2);
  beta ~ normal(0, 2);
  phi ~ exponential(1);
  y ~ poisson_log_glm(x, alpha, beta);
  y ~ neg_binomial_2_log_glm(x, alpha, beta, phi);
}
