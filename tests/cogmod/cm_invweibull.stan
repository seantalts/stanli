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

  
// Log-likelihood for one observation from the shifted inverse Weibull (Frechet) model.
// Y: observed reaction time.
// ndt: non-decision time, same unit as Y (> 0).
// poutlier: proportion of responses from the outlier process, in [0, 1].
//
// The outlier component is a half Normal with scale 0.2 s. It keeps the density
// strictly positive below `ndt`, where the shifted decision component has none.
// That is what removes the hard min-RT boundary and lets `ndt` be estimated
// directly rather than as a fraction of an observed minimum. The scale is a
// constant in SECONDS. This family expects reaction times in seconds; give it
// another unit and the component contributes nothing anywhere in the data,
// which silently reinstates the min-RT boundary it exists to remove.
//
// It is written out rather than called as normal_lpdf() because both of its
// parameters are constant: written that way, Stan recomputes the normalising
// constant for every observation on every leapfrog step.
real cogmod_invweibull_lpdf(real Y, real mu, real sigma, real ndt, real poutlier) {
    // Parameter checks
    if (mu <= 0 || sigma <= 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (Y <= 0) return negative_infinity();

    // The leading constant includes the log(2) that folds the symmetric
    // Normal onto [0, Inf).
    real lp_out = 1.3836465597893728 - 12.5 * square(Y);
    real t_adj  = Y - ndt;

    // Faster than the non-decision time: only the outlier component can have
    // produced this response.
    if (t_adj <= 0) return log(poutlier) + lp_out;

    return log_mix(poutlier, lp_out, frechet_lpdf(t_adj | mu, sigma));
}

// Log CDF and log survival of the same mixture, for brms's cens() addition
// term. See ?rcogmod_invgaussian for what censoring a reaction time means and
// when it is the right model.
real cogmod_invweibull_lcdf(real Y, real mu, real sigma, real ndt, real poutlier) {
    if (mu <= 0 || sigma <= 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (Y <= 0) return negative_infinity();
    // Outlier CDF: 2 Phi(Y / s) - 1 = erf(Y / (s sqrt(2)))
    real lF_out = log(erf(Y * 3.5355339059327369));
    real t_adj  = Y - ndt;
    if (t_adj <= 0) return log(poutlier) + lF_out;
    return log_mix(poutlier, lF_out, frechet_lcdf(t_adj | mu, sigma));
}

real cogmod_invweibull_lccdf(real Y, real mu, real sigma, real ndt, real poutlier) {
    if (mu <= 0 || sigma <= 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (Y <= 0) return 0;
    // Outlier survival: 2 Phi(-Y / s), through the lower tail (see above)
    real lS_out = 0.69314718055994529 + std_normal_lcdf(-Y * 5);
    real t_adj  = Y - ndt;
    // Not yet past the non-decision time: the decision process cannot have
    // finished, so its survival is exactly 1.
    if (t_adj <= 0) return log_mix(poutlier, lS_out, 0);
    return log_mix(poutlier, lS_out, frechet_lccdf(t_adj | mu, sigma));
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
  real min_Y = min(Y);
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
  real Intercept_sigma;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += student_t_lpdf(Intercept_sigma | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_ndt | -1.2, 0.5);
  lprior += normal_lpdf(Intercept_poutlier | -5, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigma = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    sigma += Intercept_sigma;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    mu = log1p_exp(mu);
    sigma = log1p_exp(sigma);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_invweibull_lpdf(Y[n] | mu[n], sigma[n], ndt[n], poutlier[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_sigma_Intercept = Intercept_sigma;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

