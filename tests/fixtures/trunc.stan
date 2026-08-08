// Truncation and the distribution functions it runs on. stanc3 rewrites
// the T[,] into the density minus log_diff_exp of the bounds' lcdfs, so
// this one statement covers normal_lcdf, log_diff_exp and the rewrite;
// the explicit calls below cover the lccdf and a second distribution
// whose lcdf goes through the incomplete gamma.
data {
  real y;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  y ~ normal(mu, sigma) T[0, 10];
  target += normal_lcdf(1.5 | mu, sigma);
  target += normal_lccdf(0.5 | mu, sigma);
  target += gamma_lcdf(1.2 | 2.0, sigma);
}
