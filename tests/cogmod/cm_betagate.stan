// generated with brms 2.23.0
functions {
  /* softplus link function inverse to 'log1p_exp'
   * Args:
   *   x: a positive scalar
   * Returns:
   *   a scalar in (-Inf, Inf)
   */
   real log_expm1(real x) {
     return log(expm1(x));
   }
  /* softplus link function inverse to 'log1p_exp' (vectorized)
   * Args:
   *   x: a positive vector
   * Returns:
   *   a vector in (-Inf, Inf)
   */
   vector log_expm1(vector x) {
     return log(expm1(x));
   }
  
// Log probability density function for the Beta-Gate distribution
real cogmod_betagate_lpdf(real y, real mu, real phi, real pex, real bex) {
  // Tolerance for floating point comparisons near 0 and 1
  real eps = 1e-10;

  // --- Parameter Validation ---
  // Ensure parameters are within valid ranges. Note: brms often handles this
  // via link functions, but explicit checks add robustness.
  if (!(mu > 0.0 && mu < 1.0) || !(phi > 0.0) ||
      !(pex >= 0.0 && pex <= 1.0) || !(bex >= 0.0 && bex <= 1.0) ||
      !(y >= 0.0 && y <= 1.0)) {
    return negative_infinity();
  }

  // --- Calculate Cutpoints ---
  // Calculate probability-scale cutpoints and apply logit scale
  real cutzerolog = logit(pex * (1.0 - bex));
  real cutonelog = logit(1.0 - pex * bex);

  // --- Calculate Log Probability based on y ---
  // Location parameter on logit scale
  real mu_ql = logit(mu);

  if (abs(y - 0.0) < eps) { // Case: y = 0
    // Log probability P(latent <= cutzerolog)
    return log1m_inv_logit(mu_ql - cutzerolog);

  } else if (abs(y - 1.0) < eps) { // Case: y = 1
    // Log probability P(latent > cutonelog)
    return log_inv_logit(mu_ql - cutonelog);

  } else { // Case: 0 < y < 1
    // Log probability P(cutzerolog < latent <= cutonelog)
    real log_prob_middle = log_diff_exp(log_inv_logit(mu_ql - cutzerolog),
                                        log_inv_logit(mu_ql - cutonelog));

    // Beta distribution parameters
    real shape1 = mu * phi * 2.0;
    real shape2 = (1.0 - mu) * phi * 2.0;

    // Log Beta density for y
    real log_beta_dens = beta_lpdf(y | shape1, shape2);

    // Total log probability: log(P(middle)) + log(BetaPDF(y))
    return log_prob_middle + log_beta_dens;
  }
}

}
data {
  int<lower=1> N;  // total number of observations
  vector[N] Y;  // response variable
  int<lower=1> K;  // number of population-level effects
  matrix[N, K] X;  // population-level design matrix
  int<lower=1> Kc;  // number of population-level effects after centering
  int prior_only;  // should the likelihood be ignored?
}
transformed data {
  matrix[N, Kc] Xc;  // centered version of X without an intercept
  vector[Kc] means_X;  // column means of X before centering
  for (i in 2:K) {
    means_X[i - 1] = mean(X[, i]);
    Xc[, i - 1] = X[, i] - means_X[i - 1];
  }
}
parameters {
  vector[Kc] b;  // regression coefficients
  real Intercept;  // temporary intercept for centered predictors
  real Intercept_phi;  // temporary intercept for centered predictors
  real Intercept_pex;  // temporary intercept for centered predictors
  real Intercept_bex;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_phi | 2, 1.5);
  lprior += normal_lpdf(Intercept_pex | -2, 1);
  lprior += normal_lpdf(Intercept_bex | 0, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] phi = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] pex = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] bex = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    phi += Intercept_phi;
    pex += Intercept_pex;
    bex += Intercept_bex;
    mu = inv_logit(mu);
    phi = log1p_exp(phi);
    pex = inv_logit(pex);
    bex = inv_logit(bex);
    for (n in 1:N) {
      target += cogmod_betagate_lpdf(Y[n] | mu[n], phi[n], pex[n], bex[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_phi_Intercept = Intercept_phi;
  // actual population-level intercept
  real b_pex_Intercept = Intercept_pex;
  // actual population-level intercept
  real b_bex_Intercept = Intercept_bex;
}

