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



  
// LBA defective density divided by the start-point range A, with
// z2 = z1 + delta and delta = A / (sigma * t). Both differences below vanish
// linearly in delta, so evaluating them directly and dividing by A loses every
// significant digit for a small start-point range. The Taylor expansion is used
// there instead; its truncation error is ~1e-12 at the switch, where the direct
// form still has ~12 good digits.
real cogmod_lba_dens_over_A(real drift, real sigma, real st, real z1, real delta) {
  real phi1 = exp(-0.5 * square(z1)) * 0.3989422804014327;
  if (delta < 1e-4) {
    real series = (drift + sigma * z1)
      - (delta / 2) * (drift * z1 + sigma * (square(z1) - 1))
      + (square(delta) / 6) * (drift * (square(z1) - 1)
                               + sigma * (z1 * square(z1) - 3 * z1));
    return phi1 * series / st;
  }
  real z2 = z1 + delta;
  // upper tail when both are positive, so Phi(z2) - Phi(z1) does not cancel
  real dPhi = z1 > 0 ? (Phi(-z1) - Phi(-z2)) : (Phi(z2) - Phi(z1));
  real dphi = phi1 * -expm1(-delta * (z1 + z2) / 2);
  return (drift * dPhi + sigma * dphi) / (delta * st);
}

// Log probability that an accumulator has NOT finished by t, given that its
// drift is positive: log(S - q) - log(1 - q), with S the untruncated survival
// (g(z2) - g(z1)) / delta, g(z) = z Phi(z) + phi(z), and q = Phi(-drift /
// sigma) the probability of a negative drift. With a negative drift both
// 1 - q and the untruncated CDF F are tiny, so the same number is taken as
// (1 - q) - F from the upper tail, F = (h(z2) - h(z1)) / delta with
// h(z) = z Phibar(z) - phi(z). The Taylor branches are for the cancellation in
// the quotients as delta -> 0; see .lba_lsurv_trunc() for the derivation.
real cogmod_lba_lsurv_trunc(real drift, real sigma, real z1, real delta) {
  real phi1 = exp(-0.5 * square(z1)) * 0.3989422804014327;
  real ratio = drift / sigma;
  real num;
  if (drift >= 0) {
    real s;
    if (delta < 1e-4) {
      s = Phi(z1) + (delta / 2) * phi1 - (square(delta) / 6) * z1 * phi1;
    } else {
      real z2 = z1 + delta;
      real phi2 = exp(-0.5 * square(z2)) * 0.3989422804014327;
      s = ((z2 * Phi(z2) + phi2) - (z1 * Phi(z1) + phi1)) / delta;
    }
    num = s - Phi(-ratio);
  } else {
    real f;
    if (delta < 1e-4) {
      f = Phi(-z1) - (delta / 2) * phi1 + (square(delta) / 6) * z1 * phi1;
    } else {
      real z2 = z1 + delta;
      real phi2 = exp(-0.5 * square(z2)) * 0.3989422804014327;
      f = ((z2 * Phi(-z2) - phi2) - (z1 * Phi(-z1) - phi1)) / delta;
    }
    num = Phi(ratio) - f;
  }
  if (num <= 0) return negative_infinity();
  return fmin(log(num) - std_normal_lcdf(ratio), 0);
}

// Log density of the single-accumulator LBA decision time (no shift).
real cogmod_lba1_decision_lpdf(real t, real drift, real sigma, real sigmabias, real boundary) {
  real st = fmax(sigma * t, 1e-10);
  real z1 = (boundary - drift * t) / st;
  real f = cogmod_lba_dens_over_A(drift, sigma, st, z1, sigmabias / st);
  if (f <= 0) return negative_infinity();
  return log(f) - log1m_exp(std_normal_lcdf(-drift / sigma));
}

// Defective log-density of one LBA race outcome at decision time t: the winner
// reaching the threshold just then, while the loser has not reached it at all.
//
// Each drift is a Normal truncated at zero, so the winner's density is divided
// by its own P(v > 0) and the loser's survival is taken conditional on its
// own drift being positive. Without the truncation an accumulator could draw a
// negative drift and never finish, and the density would integrate to the
// probability that at least one of them does rather than to one.
real cogmod_lba2_decision_lpdf(real t, real v_win, real s_win,
                               real v_los, real s_los,
                               real sigmabias, real boundary) {
  real stw = fmax(s_win * t, 1e-10);
  real z1w = (boundary - v_win * t) / stw;
  real f = cogmod_lba_dens_over_A(v_win, s_win, stw, z1w, sigmabias / stw);
  if (f <= 0) return negative_infinity();

  real stl = fmax(s_los * t, 1e-10);
  real z1l = (boundary - v_los * t) / stl;
  real lsurv = cogmod_lba_lsurv_trunc(v_los, s_los, z1l, sigmabias / stl);

  return log(f) - std_normal_lcdf(v_win / s_win) + lsurv;
}

// Log-likelihood for one observation from the shifted two-accumulator LBA model.
// Y: observed reaction time.
// dec: the observed choice, 0 or 1.
// mu: driftzero, the mean drift rate of accumulator 0.
// driftone: the same for accumulator 1.
// sigmazero, sigmaone: between-trial SD of each drift rate (> 0).
// sigmabias: start-point range A; the start point is z ~ Uniform(0, sigmabias).
// boundary: threshold offset, so the threshold is b = boundary + sigmabias.
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
//
// Each drift rate is a Normal truncated at zero - the convention of
// rtdists (posdrift = TRUE), DMC, EMC2 and ggdmc, so estimates are
// comparable with theirs. A negative drift would never reach the
// threshold; without the truncation the density integrates to the
// probability that at least one accumulator finishes rather than to one,
// which at low drift rates is a long way short: 0.83 at drifts of 0.5 and
// 0.2 with SDs of 1.5. `mu`, `driftone`, `sigmazero` and `sigmaone` are
// the location and scale of the UNtruncated Normal, not the mean and SD of
// the drifts actually realised.
real cogmod_lba2_lpdf(real Y, real mu, real driftone, real sigmazero, real sigmaone, real sigmabias, real boundary, real ndt, real poutlier, int dec) {
    // Parameter checks
    if (sigmazero <= 0 || sigmaone <= 0 || sigmabias < 0 || boundary <= 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
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
      ? cogmod_lba2_decision_lpdf(t_adj | mu, sigmazero, driftone, sigmaone, sigmabias, boundary)
      : cogmod_lba2_decision_lpdf(t_adj | driftone, sigmaone, mu, sigmazero, sigmabias, boundary);
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
  real Intercept_driftone;  // temporary intercept for centered predictors
  real Intercept_sigmazero;  // temporary intercept for centered predictors
  real Intercept_sigmaone;  // temporary intercept for centered predictors
  real Intercept_sigmabias;  // temporary intercept for centered predictors
  real Intercept_boundary;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0.4, 2.5);
  lprior += normal_lpdf(Intercept_driftone | 1, 2);
  lprior += normal_lpdf(Intercept_sigmazero | 0, 1);
  lprior += normal_lpdf(Intercept_sigmaone | 0, 1);
  lprior += normal_lpdf(Intercept_sigmabias | 0, 1);
  lprior += normal_lpdf(Intercept_boundary | 0, 1);
  lprior += normal_lpdf(Intercept_ndt | -1.2, 0.5);
  lprior += normal_lpdf(Intercept_poutlier | -5, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] driftone = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmazero = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmaone = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmabias = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] boundary = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    driftone += Intercept_driftone;
    sigmazero += Intercept_sigmazero;
    sigmaone += Intercept_sigmaone;
    sigmabias += Intercept_sigmabias;
    boundary += Intercept_boundary;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    sigmazero = log1p_exp(sigmazero);
    sigmaone = log1p_exp(sigmaone);
    sigmabias = log1p_exp(sigmabias);
    boundary = log1p_exp(boundary);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_lba2_lpdf(Y[n] | mu[n], driftone[n], sigmazero[n], sigmaone[n], sigmabias[n], boundary[n], ndt[n], poutlier[n], dec[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_driftone_Intercept = Intercept_driftone;
  // actual population-level intercept
  real b_sigmazero_Intercept = Intercept_sigmazero;
  // actual population-level intercept
  real b_sigmaone_Intercept = Intercept_sigmaone;
  // actual population-level intercept
  real b_sigmabias_Intercept = Intercept_sigmabias;
  // actual population-level intercept
  real b_boundary_Intercept = Intercept_boundary;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

