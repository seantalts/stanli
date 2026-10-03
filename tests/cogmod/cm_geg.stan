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

  
// log(Phi(x)), the one piece of arithmetic every normal tail in this package is
// built from. Neither of Stan's two routes to it is good enough for both jobs
// it has here, which is to be right in the far tail *and* to hand back a
// usable derivative there. All three claims below were measured against
// central differences of the log probability over 20000 responses.
//
// The erfc route - std_normal_lcdf() is not it, but lognormal_lcdf(),
// lognormal_lccdf() and the log(u1) + log1m(u2 / u1) the LogNormal used to
// write are - has good partials, to about 4e-6 on a summed gradient of order
// 1e3. But erfc underflows near x = -38, and then the value is log(0) and the
// partials are inf or 0/0. That is not a harmless -inf in a mixture:
// log_mix() in the lpdf stays finite when the decision component is -inf, but
// reverse mode multiplies the (zero) adjoint into the stored partial, and
// 0 * inf is NaN, so a single response turns the gradient of the whole model
// to NaN - 'Gradient evaluated at the initial value is not finite' at the
// start of a fit, divergent transitions afterwards.
//
// std_normal_lcdf() has the range: its value is exact against R's
// pnorm(log.p = TRUE) as far as x = -1e7. Its partials are not - they sat
// 1.7e-3 from central differences where the erfc route sat 4e-6 on the LNR,
// and 2e-4 to 7e-2 on the RDM, which took every tail through it - so it is not
// a drop-in for the tails of a race, where those partials are the gradient of
// the drifts and the scales.
//
// So: erfc in the body of the distribution, and below x = -25 the asymptotic
// expansion of the tail,
//
//   Phi(x) = phi(x) / (-x) * (1 - 1/x^2 + 3/x^4 - 15/x^6 + 105/x^8 - 945/x^10)
//
// whose leading term is the exponent itself. Nothing underflows, the result
// stays finite and differentiable as far as x = -1e150, and the six terms
// agree with pnorm(log.p = TRUE) to 4e-16 relative from x = -25 down - the
// last bit of a double - so the two branches meet with no step in the density.
real cogmod_log_Phi(real x) {
  if (x < -25) {
    real z = inv_square(x);
    real series = 1 + z * (-1 + z * (3 + z * (-15 + z * (105 - 945 * z))));
    return -0.5 * square(x) - log(-x) - 0.91893853320467274 + log(series);
  }
  if (x > 0) return log1p(-0.5 * erfc(x * 0.7071067811865476));
  return log(0.5 * erfc(-x * 0.7071067811865476));
}

// log f(y) for the ex-Gaussian. With z = (y - mu) / sigma this is
//
//   lb - log(tau),   lb = sigma^2 / (2 tau^2) - (y - mu) / tau
//                           + log Phi(z - sigma / tau),
//
// the same expression dcogmod_exgaussian() evaluates in R and the same one
// Stan's exp_mod_normal_lpdf() does, over cogmod_log_Phi() rather than the
// built-in's bare erfc: identical in the body of the distribution, and still
// finite about 38 standardized units into the left tail, where erfc is not.
real cogmod_exgaussian_ldens(real y, real mu, real sigma, real tau) {
  return square(sigma) / (2 * square(tau)) - (y - mu) / tau - log(tau)
           + cogmod_log_Phi((y - mu) / sigma - sigma / tau);
}

// log F(y) = log(Phi(z) - exp(lb)), with `lb` as above - note that the term F
// subtracts IS the density, up to the 1 / tau, which is what cogmod_geg_lpdf()
// exploits (see the note there).
//
// The difference is taken with log1m_exp() rather than by subtracting the two
// terms, because in the left tail they are individually tiny and very close
// together - exactly where cogmod_geg() needs the CDF, since shape < 1
// multiplies log F by a negative number and any error there is amplified.
// F > 0 everywhere, so the difference is positive; the floor on `d` is against
// rounding alone, it is the same one .lcdf_exgaussian() uses so that the two
// sides agree bit for bit rather than one of them returning -inf, and it is
// what leaves the sum negative without a closing fmin().
real cogmod_exgaussian_logcdf(real y, real mu, real sigma, real tau) {
  real z = (y - mu) / sigma;
  real la = cogmod_log_Phi(z);
  real lb = square(sigma) / (2 * square(tau)) - (y - mu) / tau
              + cogmod_log_Phi(z - sigma / tau);
  real d = fmin(lb - la, -2.220446049250313e-16);
  return la + log1m_exp(d);
}

// log S(y), as the SUM of the two positive terms
//   S(y) = Phi(-z) + exp(sigma^2 / (2 tau^2) - (y - mu) / tau) Phi(z - sigma / tau),
// rather than log1m_exp() of the above: a right-censored slow response sits
// exactly where 1 - F has no digits left. Each tail is written where its terms
// do not cancel.
real cogmod_exgaussian_logsurv(real y, real mu, real sigma, real tau) {
  real z = (y - mu) / sigma;
  return fmin(log_sum_exp(
    cogmod_log_Phi(-z),
    square(sigma) / (2 * square(tau)) - (y - mu) / tau
      + cogmod_log_Phi(z - sigma / tau)
  ), 0);
}

// Log-likelihood for a single observation from the Generalised Ex-Gaussian.
// Y: observed reaction time.
// mu: location of the Gaussian component. Unbounded, as for the ex-Gaussian.
// sigma: SD of the Gaussian component (> 0).
// tau: mean of the exponential component (> 0).
// shape: power applied to the ex-Gaussian CDF (> 0). shape = 1 is the
//        ex-Gaussian exactly, and the two lines below then reduce to
//        exp_mod_normal_lpdf alone.
real cogmod_geg_lpdf(real Y, real mu, real sigma, real tau, real shape) {
    // Parameter checks
    if (sigma <= 0 || tau <= 0 || shape <= 0) return negative_infinity();

    // The alpha-power construction
    //     f(x) = shape * F(x)^(shape - 1) * f(x)
    // needs the ex-Gaussian's density AND its CDF at every evaluation. Neither
    // comes from Stan's exp_mod_normal here: its lcdf() has the value but not
    // the partials, and `shape - 1` multiplies that term straight into the
    // gradient.
    //
    // The two lines below are cogmod_exgaussian_logcdf() and
    // cogmod_exgaussian_ldens() written out rather than called, because the
    // density is the CDF's second term over tau - `lb` serves both - and
    // calling the two would evaluate cogmod_log_Phi(z - sigma / tau) twice.
    // Returning the pair from one function instead was measured and is worse
    // still; see .EXGAUSSIAN_STAN_PRELUDE in model_exgaussian.R. The R side
    // (dcogmod_geg()) keeps the redundancy, being vectorised and not
    // differentiated.
    real z = (Y - mu) / sigma;
    real la = cogmod_log_Phi(z);
    real lb = square(sigma) / (2 * square(tau)) - (Y - mu) / tau
                + cogmod_log_Phi(z - sigma / tau);
    real d = fmin(lb - la, -2.220446049250313e-16);
    return log(shape) + (shape - 1) * (la + log1m_exp(d)) + lb - log(tau);
}

// CDF and survival, for brms's cens() addition term (see ?rcogmod_invgaussian
// for what censoring a reaction time means). log F_GEG = shape * log F_EG is
// the construction itself; the survival has no positive-terms form of its own,
// so it is the log1m_exp of that.
real cogmod_geg_lcdf(real Y, real mu, real sigma, real tau, real shape) {
    if (sigma <= 0 || tau <= 0 || shape <= 0) return negative_infinity();
    return shape * cogmod_exgaussian_logcdf(Y, mu, sigma, tau);
}

real cogmod_geg_lccdf(real Y, real mu, real sigma, real tau, real shape) {
    if (sigma <= 0 || tau <= 0 || shape <= 0) return negative_infinity();
    real lF = shape * cogmod_exgaussian_logcdf(Y, mu, sigma, tau);
    return lF < 0 ? log1m_exp(lF) : negative_infinity();
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
  real Intercept_sigma;  // temporary intercept for centered predictors
  real Intercept_tau;  // temporary intercept for centered predictors
  real Intercept_shape;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += normal_lpdf(Intercept | 0.4, 0.25);
  lprior += normal_lpdf(Intercept_sigma | -2.3, 0.7);
  lprior += normal_lpdf(Intercept_tau | -1.5, 0.7);
  lprior += normal_lpdf(Intercept_shape | 0, 0.5);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigma = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] tau = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] shape = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    sigma += Intercept_sigma;
    tau += Intercept_tau;
    shape += Intercept_shape;
    sigma = log1p_exp(sigma);
    tau = log1p_exp(tau);
    shape = exp(shape);
    for (n in 1:N) {
      target += cogmod_geg_lpdf(Y[n] | mu[n], sigma[n], tau[n], shape[n]);
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
  real b_tau_Intercept = Intercept_tau;
  // actual population-level intercept
  real b_shape_Intercept = Intercept_shape;
}

