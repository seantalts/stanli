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

// log(Phi(y + c) - Phi(y)) for c > 0, from whichever tail keeps the two terms
// from cancelling: the upper one when y > 0, where both CDFs sit near 1, the
// lower one otherwise. Taken as the larger tail plus log(1 - ratio) in log
// space, the way .lognormal_ldiff_pnorm() does it - the quotient u2 / u1 this
// replaces divides two minute numbers and lost its own accuracy long before
// either underflowed (8e-4 against central differences, against 1e-6 here).
real cogmod_lognormal_ldiff_Phi(real y, real c) {
  real hi;
  real lo;
  if (y > 0) {
    hi = cogmod_log_Phi(-y);
    lo = cogmod_log_Phi(-(y + c));
  } else {
    hi = cogmod_log_Phi(y + c);
    lo = cogmod_log_Phi(y);
  }
  return lo < hi ? hi + log1m_exp(lo - hi) : negative_infinity();
}

// Log density of the accumulator's finishing time with start-point range A.
// At A = 0 this is the LogNormal itself, at the LogNormal's cost.
real cogmod_lognormal_acc_ldens(real t, real meanlog, real sigma, real A) {
  if (A == 0) return lognormal_lpdf(t | meanlog, sigma);
  real a = (meanlog - log(t)) / sigma;
  real c = log1p(A) / sigma;
  real x = a - sigma;
  if (c < 1e-4) {
    real series = 1 - c * x / 2 + square(c) * (square(x) - 1) / 6;
    if (series <= 0) return negative_infinity();
    return lognormal_lpdf(t | meanlog, sigma) + log(log1p(A) / A) + log(series);
  }
  return -meanlog + square(sigma) / 2 + cogmod_lognormal_ldiff_Phi(x, c) - log(A);
}

// [log F, log S] of the accumulator's finishing time, each computed directly
// on the side where it is the small one. See .lognormal_acc_ltails().
vector cogmod_lognormal_acc_ltails(real t, real meanlog, real sigma, real A) {
  real a = (meanlog - log(t)) / sigma;
  // At A = 0 this is the plain LogNormal, whose two tails are Phi(-a) and
  // Phi(a). Written that way rather than as lognormal_lcdf()/lognormal_lccdf(),
  // which are erfc alone and so reach log(0) with non-finite partials around
  // |a| = 38 - see cogmod_log_Phi() above for what that costs.
  if (A == 0) return [cogmod_log_Phi(-a), cogmod_log_Phi(a)]';
  real c = log1p(A) / sigma;
  if (c < 1e-4) {
    real r = log1p(A) / A;
    real corr = c * r * (0.5 + c * (2 * sigma - a) / 6);
    real lPa = cogmod_log_Phi(a);
    real lQa = cogmod_log_Phi(-a);
    real lphi = std_normal_lpdf(a);
    real lS = fmin(lPa + log1p(corr * exp(lphi - lPa)), 0);
    real dF = 1 - corr * exp(lphi - lQa);
    real lF = dF > 0 ? fmin(lQa + log(dF), 0) : negative_infinity();
    return [lF, lS]';
  }
  real lA = log(A);
  real lD1 = cogmod_lognormal_ldiff_Phi(a, c) - lA;
  real lD2 = -meanlog + square(sigma) / 2 + log(t)
             + cogmod_lognormal_ldiff_Phi(a - sigma, c) - lA;
  if (a < 0) {
    real lP = cogmod_log_Phi(a + c);
    real br = 1 + exp(lD1 - lP) - exp(lD2 - lP);
    if (br <= 0) return [0, negative_infinity()]';
    real lS = fmin(lP + log(br), 0);
    return [lS < 0 ? log1m_exp(lS) : negative_infinity(), lS]';
  }
  real lQ = cogmod_log_Phi(-a);
  real R = exp(cogmod_log_Phi(-a - c) - lQ);
  real br = R - (1 - R) / A + exp(lD2 - lQ);
  if (br <= 0) return [negative_infinity(), 0]';
  real lF = fmin(lQ + log(br), 0);
  return [lF, lF < 0 ? log1m_exp(lF) : negative_infinity()]';
}

// Above A = 0 the two tails share lD1 and lD2, so building the pair and taking
// one of them is the cheap way round. At A = 0 they share nothing - each is a
// single cogmod_log_Phi() of the same standardized time - and the pair would
// put a whole discarded tail on the autodiff tape for every observation.
// cogmod_lnr() reads the survival alone, once per trial per loser, so that is
// the hot path of the family.
real cogmod_lognormal_acc_logcdf(real t, real meanlog, real sigma, real A) {
  if (A == 0) return cogmod_log_Phi((log(t) - meanlog) / sigma);
  return cogmod_lognormal_acc_ltails(t, meanlog, sigma, A)[1];
}

real cogmod_lognormal_acc_logsurv(real t, real meanlog, real sigma, real A) {
  if (A == 0) return cogmod_log_Phi((meanlog - log(t)) / sigma);
  return cogmod_lognormal_acc_ltails(t, meanlog, sigma, A)[2];
}

// Defective log-density of one LNR race outcome at decision time t: the winner
// finishing just then while the loser has not finished at all. Each accumulator
// is cogmod_lognormal()'s, with meanlog = -nu.
real cogmod_lnr_decision_lpdf(real t, real nu_win, real s_win,
                              real nu_los, real s_los, real sigmabias) {
  return cogmod_lognormal_acc_ldens(t, -nu_win, s_win, sigmabias)
       + cogmod_lognormal_acc_logsurv(t, -nu_los, s_los, sigmabias);
}

// Log-likelihood for one observation from the shifted Log-Normal Race model.
// Y: observed reaction time.
// dec: the observed choice, 0 or 1.
// mu: nuzero, the processing speed of accumulator 0 (meanlog = -mu, so larger is faster).
// nuone: the same for accumulator 1.
// sigmazero, sigmaone: the log-scale SD of each accumulator (> 0).
// sigmabias: start-point range, in units of the threshold offset (>= 0); 0 is the plain LNR.
// ndt: non-decision time, same unit as Y (> 0).
// poutlier: proportion of responses from the outlier process, in [0, 1].
//
// The outlier component is a guess: the choice is uniform over the 2 response
// options and the RT is a half Normal with scale 0.2 s. It keeps the density
// strictly positive below `ndt`, where the shifted decision component has none.
// That is what removes the hard min-RT boundary and lets `ndt` be estimated
// directly rather than as a fraction of an observed minimum. The scale is a
// constant in SECONDS. This family expects reaction times in seconds; give it
// another unit and the component contributes nothing anywhere in the data,
// which silently reinstates the min-RT boundary it exists to remove.
//
// The 1 / 2 is what keeps the total density summing to one over the response
// options; without it it comes to 1 + poutlier. The leading constant below
// carries it, along with the normalising constant of the Normal and the
// log(2) that folds it onto [0, Inf) - all of them constant, so writing them
// out saves Stan recomputing them for every observation on every leapfrog step.
real cogmod_lnr_lpdf(real Y, real mu, real nuone, real sigmazero, real sigmaone, real sigmabias, real ndt, real poutlier, int dec) {
    // Parameter checks
    if (sigmazero <= 0 || sigmaone <= 0 || sigmabias < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (dec < 0 || dec > 1) return negative_infinity();
    if (Y <= 0) return negative_infinity();

    real lp_out = 0.6904993792294275 - 12.5 * square(Y);
    real t_adj  = Y - ndt;

    // Faster than the non-decision time: only the outlier component can have
    // produced this response.
    if (t_adj <= 0) return log(poutlier) + lp_out;

    real lp_dec = dec == 0
      ? cogmod_lnr_decision_lpdf(t_adj | mu, sigmazero, nuone, sigmaone, sigmabias)
      : cogmod_lnr_decision_lpdf(t_adj | nuone, sigmaone, mu, sigmazero, sigmabias);
    return log_mix(poutlier, lp_out, lp_dec);
}

}
data {
  int<lower=1> N;  // total number of observations
  vector[N] Y;  // response variable
  array[N] int<lower=0,upper=1> dec;  // decisions
  int<lower=1> K;  // number of population-level effects
  matrix[N, K] X;  // population-level design matrix
  int<lower=1> Kc;  // number of population-level effects after centering
  int<lower=1> K_nuone;  // number of population-level effects
  matrix[N, K_nuone] X_nuone;  // population-level design matrix
  int<lower=1> Kc_nuone;  // number of population-level effects after centering
  int<lower=1> K_ndt;  // number of population-level effects
  matrix[N, K_ndt] X_ndt;  // population-level design matrix
  int<lower=1> Kc_ndt;  // number of population-level effects after centering
  int prior_only;  // should the likelihood be ignored?
}
transformed data {
  real min_Y = min(Y);
  matrix[N, Kc] Xc;  // centered version of X without an intercept
  vector[Kc] means_X;  // column means of X before centering
  matrix[N, Kc_nuone] Xc_nuone;  // centered version of X_nuone without an intercept
  vector[Kc_nuone] means_X_nuone;  // column means of X_nuone before centering
  matrix[N, Kc_ndt] Xc_ndt;  // centered version of X_ndt without an intercept
  vector[Kc_ndt] means_X_ndt;  // column means of X_ndt before centering
  for (i in 2:K) {
    means_X[i - 1] = mean(X[, i]);
    Xc[, i - 1] = X[, i] - means_X[i - 1];
  }
  for (i in 2:K_nuone) {
    means_X_nuone[i - 1] = mean(X_nuone[, i]);
    Xc_nuone[, i - 1] = X_nuone[, i] - means_X_nuone[i - 1];
  }
  for (i in 2:K_ndt) {
    means_X_ndt[i - 1] = mean(X_ndt[, i]);
    Xc_ndt[, i - 1] = X_ndt[, i] - means_X_ndt[i - 1];
  }
}
parameters {
  vector[Kc] b;  // regression coefficients
  real Intercept;  // temporary intercept for centered predictors
  vector[Kc_nuone] b_nuone;  // regression coefficients
  real Intercept_nuone;  // temporary intercept for centered predictors
  real Intercept_sigmazero;  // temporary intercept for centered predictors
  real Intercept_sigmaone;  // temporary intercept for centered predictors
  vector[Kc_ndt] b_ndt;  // regression coefficients
  real Intercept_ndt;  // temporary intercept for centered predictors
  real<lower=0,upper=1> poutlier;
}
transformed parameters {
  real sigmabias = 0;
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0.6, 2.5);
  lprior += normal_lpdf(b_nuone | 0, 0.5);
  lprior += normal_lpdf(Intercept_nuone | 0.7, 1.5);
  lprior += normal_lpdf(Intercept_sigmazero | 0, 1);
  lprior += normal_lpdf(Intercept_sigmaone | 0, 1);
  lprior += normal_lpdf(b_ndt | 0, 0.2);
  lprior += normal_lpdf(Intercept_ndt | -1.2, 0.5);
  lprior += exponential_lpdf(poutlier | 100)
    - 1 * exponential_lcdf(1 | 100);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] nuone = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmazero = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmaone = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    nuone += Intercept_nuone + Xc_nuone * b_nuone;
    sigmazero += Intercept_sigmazero;
    sigmaone += Intercept_sigmaone;
    ndt += Intercept_ndt + Xc_ndt * b_ndt;
    sigmazero = log1p_exp(sigmazero);
    sigmaone = log1p_exp(sigmaone);
    ndt = exp(ndt);
    for (n in 1:N) {
      target += cogmod_lnr_lpdf(Y[n] | mu[n], nuone[n], sigmazero[n], sigmaone[n], sigmabias, ndt[n], poutlier, dec[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_nuone_Intercept = Intercept_nuone - dot_product(means_X_nuone, b_nuone);
  // actual population-level intercept
  real b_sigmazero_Intercept = Intercept_sigmazero;
  // actual population-level intercept
  real b_sigmaone_Intercept = Intercept_sigmaone;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt - dot_product(means_X_ndt, b_ndt);
}

