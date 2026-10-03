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
  
// Normalizing constant of the standardized log-gamma with shape k = 1 / shape^2,
// with its leading k already cancelled against the equal and opposite k hidden
// in the kernel below. Doing that cancellation on paper is what keeps the
// density accurate near shape = 0, where both terms reach 1e8 by |shape| = 1e-4.
// Below |shape| = 1e-3 the Stirling series is used, where 1 / (12k) = shape^2 / 12; the
// two branches agree to about 1e-9 where they meet, so the density is smooth
// through the switch and finite at shape = 0 exactly.
real cogmod_loggamma_lconst(real shape) {
  if (abs(shape) < 1e-3) {
    return -0.5 * log(2 * pi()) - square(shape) / 12 + pow(shape, 6) / 360;
  }
  real k = inv_square(shape);
  return (k - 0.5) * log(k) - lgamma(k) - k;
}

// (shape * w - expm1(shape * w)) / shape^2, the log-gamma kernel with the same k removed.
// At shape = 0 this is exactly -w^2 / 2, the Normal kernel, which is why shape = 0
// gives back the LogNormal rather than approaching it.
real cogmod_loggamma_lkernel(real shape, real w) {
  real u = shape * w;
  if (abs(u) < 1e-6) {
    return -0.5 * square(w) - shape * pow(w, 3) / 6 - square(shape) * pow(w, 4) / 24;
  }
  return (u - expm1(u)) / square(shape);
}

// Log-likelihood for one observation from the shifted Log-Gamma model.
// Y: observed reaction time.
// mu: location of the decision time on the log scale.
// sigma: scale of the decision time on the log scale (> 0).
// shape: shape of the log-gamma on the log-RT scale (unconstrained). shape = 0 is the
//    LogNormal, shape = sigma the Gamma, shape = 1 the Weibull.
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
//
// Note that when sigma * shape >= 1 the decision density is unbounded at `ndt`, so
// the likelihood is unbounded too; the prior on shape is what keeps the sampler out
// of that region. See ?rcogmod_loggamma.
real cogmod_loggamma_lpdf(real Y, real mu, real sigma, real shape, real ndt, real poutlier) {
    // Parameter checks
    if (sigma <= 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
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

    return log_mix(poutlier, lp_out, -log(sigma) - log(t_adj) + cogmod_loggamma_lconst(shape) + cogmod_loggamma_lkernel(shape, (log(t_adj) - mu) / sigma));
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
  real Intercept_shape;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0.7, 2.5);
  lprior += student_t_lpdf(Intercept_sigma | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_shape | 0, 0.5);
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
    vector[N] shape = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    sigma += Intercept_sigma;
    shape += Intercept_shape;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    sigma = log1p_exp(sigma);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_loggamma_lpdf(Y[n] | mu[n], sigma[n], shape[n], ndt[n], poutlier[n]);
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
  real b_shape_Intercept = Intercept_shape;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

