// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters {
  real<lower=0.2> V;
  real<lower=0.2> W;
  real m0;
  real<lower=0.2> C0;
  real g;
  real y1;
}
model {
  real marginal_variance = g ^ 2 * C0 + W + V;
  real lp = gaussian_dlm_obs_lpdf([[y1]] | [[1]], [[g]], [[V]], [[W]], [m0]', [[C0]]);
  if (abs(lp - normal_lpdf(y1 | g * m0, sqrt(marginal_variance))) > 1e-9)
    reject("gaussian_dlm_obs");
  real lp_vector = gaussian_dlm_obs_lpdf([[y1]] | [[1]], [[g]], [V]', [[W]], [m0]', [[C0]]);
  if (abs(lp - lp_vector) > 1e-12) reject("gaussian_dlm_obs vector V");
  target += -0.5 * square(g) - 0.5 * square(m0) - 0.5 * square(y1);
  target += gaussian_dlm_obs_lpdf([[y1]] | [[1]], [[g]], [[V]], [[W]], [m0]', [[C0]]);
}
