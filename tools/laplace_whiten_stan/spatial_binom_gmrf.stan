// Binomial GLMM with a centred GMRF field, Q = tau * (kappa^2 I + L).
// lambda holds the eigenvalues of the grid Laplacian L, so log det Q is cheap.
data {
  int<lower=1> N;
  int<lower=1> N_edges;
  array[N_edges] int<lower=1, upper=N> node1;
  array[N_edges] int<lower=1, upper=N> node2;
  vector<lower=0>[N] lambda;
  vector[N] x;
  array[N] int<lower=1> trials;
  array[N] int<lower=0> y;
}
parameters {
  real alpha;
  real beta;
  real<lower=0> tau;
  real<lower=0> kappa;
  vector[N] phi;
}
model {
  alpha ~ normal(0, 2);
  beta ~ normal(0, 1);
  tau ~ lognormal(log(2), 1);
  kappa ~ lognormal(log(0.3), 0.5);
  target += 0.5 * (N * log(tau) + sum(log(square(kappa) + lambda)))
            - 0.5 * tau * (square(kappa) * dot_self(phi) + dot_self(phi[node1] - phi[node2]));
  y ~ binomial_logit(trials, alpha + beta * x + phi);
}
