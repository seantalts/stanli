// Gaussian on the unconstrained scale, so Laplace, Pathfinder and full-rank
// ADVI are exact up to Monte Carlo error and mean-field ADVI is wrong in a
// known way: theta is correlated, and log(tau) is normal(0, 0.5).
data {
  vector[2] m;
  cov_matrix[2] S;
}
parameters {
  vector[2] theta;
  real<lower=0> tau;
}
model {
  theta ~ multi_normal(m, S);
  tau ~ lognormal(0, 0.5);
}
generated quantities {
  real tau2 = square(tau);
  real noise = normal_rng(theta[1], 1);
}
