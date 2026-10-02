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

  
// The decision component: Stan's Wiener density at the decision time. `w` is
// the starting point of whichever boundary is being scored, so the caller flips
// both it and the drift for the lower one.
//
// The shift is applied by the mixture around this, so the diffusion's own
// non-decision time should be zero - but wiener_lpdf() rejects that outright,
// and the density depends on the time and the non-decision time only through
// their difference. So both are offset by the same 1e-10, exactly as the R side
// offsets them (see .DDM_TAU0). It is a workaround for an argument check rather
// than an approximation: at that magnitude the offset cancels to the last bit.
//
// sigmabias is a fraction of the widest start-point range that keeps the start
// point inside the boundaries, so sw = sigmabias * fmin(2w, 2(1-w)) - which is
// the same number for w and 1 - w, as it must be. sigmandt is st0 itself, in
// the same unit as Y, with the shift as its lower bound.
//
// Performance note: Stan's own `sw == 0 && st0 == 0` fast path inside
// `wiener_lpdf()` still delegates to the newer 'wiener5' algorithm (which also
// handles sv analytically), a structurally different and empirically much
// costlier implementation than the classic 4-parameter `wiener_lpdf()`, even
// when sv = 0. So we short-circuit ourselves whenever sw and st0 both vanish:
// to the classic 4-parameter Navarro & Fuss density when sv is also 0 (the
// fastest path), or to the dedicated 5-parameter (sv-only) form otherwise,
// which is still much cheaper than the general 7-parameter form (the latter
// falls back to adaptive numerical quadrature whenever sw or st0 is nonzero).
// Measured per observation and gradient (2026-09-18): 2.0 us classic, 5.4 us
// with sv, 100 us with one of sw / st0 nonzero, 274 us with both - and the
// last is what the tolerance passed to it (see .DDM_WIENER_PRECISION) brings
// down to about 160. The test is for *exact* zero, so an estimated sigmabias
// or sigmandt never takes the fast path however small it gets; only fixing
// it in bf() does.
//
// The classic form is not usable everywhere, though - see
// cogmod_ddm_log_density_scale() below for the two regions it has to be kept
// out of, and why keeping it out of them is what removes the "Non-finite
// gradient" reports.

// A cheap estimate of the log first-passage density, used only to *route*
// between Stan's implementations - never as the density itself. Both call
// sites below fall back to an implementation that computes the exact value, so
// this being off in either direction costs a little speed and nothing else.
//
// Stan's classic 4-parameter `wiener_lpdf()` returns -inf in two regions, and
// hands back NaN partial derivatives when it does. A NaN partial is not made
// harmless by the mixture weight on it being zero: reverse-mode multiplies the
// (zero) adjoint into the stored partial, and 0 * NaN is NaN, so a single such
// trial turns the gradient of the whole model to NaN. That is what "Error
// evaluating model log probability: Non-finite gradient" reports during a
// Pathfinder search, and it is also why such a fit collects divergent
// transitions - a proposal whose gradient is not finite is rejected as one.
//
// The first region is the density underflowing to zero, which this function
// detects: the two series Navarro & Fuss give for the driftless, unit-boundary
// density at rescaled time tau = t / boundary^2 are each exact in their own
// tail and a wild extrapolation outside it, so the smaller of the two leading
// terms is the one to believe. Integrating a normal drift out of the density
// is closed-form, so sv enters as a factor rather than as another series. The
// second region is the alternating small-time series losing its sum to
// cancellation while the density is still perfectly representable; that one
// depends on tau alone - not on the drift or the scale separately - and is
// tested for at the call site.
//
// The general 7-parameter density averages over a start point spread across
// sw and a non-decision time spread across st0, so what matters there is
// whether *any* point of those ranges has a representable density. Each term
// is therefore taken at whichever end of each range makes it largest, which
// reduces to the point value when both ranges vanish.
real cogmod_ddm_log_density_scale(real t, real v, real boundary, real w,
                                  real sv, real sw, real st0) {
  real w_lo = fmax(w - sw / 2, 1e-12);
  real w_hi = fmin(w + sw / 2, 1 - 1e-12);
  real t_lo = fmax(t - st0, 1e-12);
  real tau_hi = t / square(boundary);
  real tau_lo = t_lo / square(boundary);

  // The small-time term rises with the decision time and peaks at a start
  // point of sqrt(tau); the large-time term falls with the decision time and
  // peaks at the start point nearest the midpoint.
  real ws = fmin(fmax(sqrt(tau_hi), w_lo), w_hi);
  real small = log(ws) - square(ws) / (2 * tau_hi)
               - 0.5 * log(2 * pi() * tau_hi^3);
  real wl = fmin(fmax(0.5, w_lo), w_hi);
  real large = log(pi()) - square(pi()) * tau_lo / 2 + log(sin(pi() * wl));

  // The drift factor, which at sv = 0 is just exp(-v * boundary * w -
  // v^2 * t / 2). Dropping the -log(sqrt(1 + sv^2 t)) that goes with it only
  // makes the estimate larger, which is the harmless direction.
  real num = -square(v) * t_lo - 2 * v * boundary * (v > 0 ? w_lo : w_hi)
             + square(sv * boundary * w_hi);
  real drift = num / (2 * (1 + square(sv) * (num > 0 ? t_lo : t)));

  return -2 * log(boundary) + fmin(small, large) + drift;
}

real cogmod_ddm_decision_lpdf(real t, real v, real boundary, real w,
                              real sigmadrift, real sigmabias, real sigmandt) {
  real tau0 = 1e-10;
  // Below the offset the Wiener density has underflowed to zero anyway, so
  // -inf is the answer to double precision as well as the safe one.
  if (t <= tau0) return negative_infinity();
  real y = t + tau0;
  real sw = sigmabias * fmin(2 * w, 2 * (1 - w));

  if (sw == 0 && sigmandt == 0) {
    // The classic density, but only where it is sound. Cancellation sets in
    // below tau = 6.6e-4 (measured over w from 1e-6 to 1 - 1e-6 and |v| up to
    // 15, and invariant in the boundary and the drift), so the test keeps a
    // factor of 15 clear of it; from tau = 0.002 upwards the classic and the
    // sv-capable implementations agree to 1e-13, so there is no step in the
    // density where the two paths meet. The second test stops about 100 log
    // units short of underflow - far more slack than the estimate's error.
    if (sigmadrift == 0 && t > 0.01 * square(boundary)
        && cogmod_ddm_log_density_scale(t, v, boundary, w, 0, 0, 0) > -600) {
      return wiener_lpdf(y | boundary, tau0, w, v);
    }
    // wiener5 works in log space throughout: it stays finite, with finite
    // gradients, down to log-densities of -1e7 and beyond, so it needs no
    // guard of its own.
    return wiener_lpdf(y | boundary, tau0, w, v, sigmadrift);
  }
  // The 7-parameter form has the same -inf-with-NaN-partials failure as the
  // classic one and no sounder implementation to fall back on, so the answer
  // here is the -inf it would have returned anyway - only as a constant, which
  // carries no partial derivatives to poison the gradient with.
  if (cogmod_ddm_log_density_scale(t, v, boundary, w, sigmadrift, sw,
                                   sigmandt) < -600) {
    return negative_infinity();
  }
  return wiener_lpdf(y | boundary, tau0, w, v, sigmadrift, sw, sigmandt, 0.001);
}

// Log-likelihood for one observation from the shifted Drift Diffusion model.
// Y: observed reaction time.
// dec: the observed choice. 1 is the UPPER boundary, 0 the lower.
// mu: drift rate, positive towards the boundary coded 1.
// boundary: boundary separation (> 0).
// bias: starting point as a proportion of `boundary`, in (0, 1).
// sigmadrift: between-trial SD of the drift rate (sv, >= 0).
// sigmabias: between-trial start-point range as a fraction in [0, 1) of the widest
//    range that keeps the start point inside the boundaries.
// sigmandt: between-trial range of the non-decision time (st0), in the same unit as Y,
//    with `ndt` its lower bound.
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
// sigmandt is st0 itself, in seconds, not a fraction of anything.
real cogmod_ddm_lpdf(real Y, real mu, real boundary, real bias, real sigmadrift, real sigmabias, real sigmandt, real ndt, real poutlier, int dec) {
    // Parameter checks
    if (boundary <= 0 || bias <= 0 || bias >= 1 || sigmadrift < 0 || sigmabias < 0 || sigmabias >= 1 || sigmandt < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (dec < 0 || dec > 1) return negative_infinity();
    if (Y <= 0) return negative_infinity();

    real lp_out = 0.6904993792294275 - 12.5 * square(Y);
    real t_adj  = Y - ndt;

    // Faster than the non-decision time: only the outlier component can have
    // produced this response.
    if (t_adj <= 0) return log(poutlier) + lp_out;

    real lp_dec = dec == 1
      ? cogmod_ddm_decision_lpdf(t_adj | mu, boundary, bias, sigmadrift, sigmabias, sigmandt)
      : cogmod_ddm_decision_lpdf(t_adj | -mu, boundary, 1 - bias, sigmadrift, sigmabias, sigmandt);
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
  real Intercept_boundary;  // temporary intercept for centered predictors
  real Intercept_bias;  // temporary intercept for centered predictors
  real Intercept_sigmadrift;  // temporary intercept for centered predictors
  real Intercept_sigmabias;  // temporary intercept for centered predictors
  real Intercept_sigmandt;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0.4, 2.5);
  lprior += logistic_lpdf(Intercept_bias | 0, 1);
  lprior += normal_lpdf(Intercept_sigmadrift | 0, 1);
  lprior += normal_lpdf(Intercept_sigmabias | -2, 1);
  lprior += normal_lpdf(Intercept_sigmandt | -3, 1);
  lprior += normal_lpdf(Intercept_ndt | -1.2, 0.5);
  lprior += normal_lpdf(Intercept_poutlier | -5, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] boundary = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] bias = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmadrift = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmabias = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmandt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    boundary += Intercept_boundary;
    bias += Intercept_bias;
    sigmadrift += Intercept_sigmadrift;
    sigmabias += Intercept_sigmabias;
    sigmandt += Intercept_sigmandt;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    boundary = log1p_exp(boundary);
    bias = inv_logit(bias);
    sigmadrift = log1p_exp(sigmadrift);
    sigmabias = inv_logit(sigmabias);
    sigmandt = exp(sigmandt);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_ddm_lpdf(Y[n] | mu[n], boundary[n], bias[n], sigmadrift[n], sigmabias[n], sigmandt[n], ndt[n], poutlier[n], dec[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_boundary_Intercept = Intercept_boundary;
  // actual population-level intercept
  real b_bias_Intercept = Intercept_bias;
  // actual population-level intercept
  real b_sigmadrift_Intercept = Intercept_sigmadrift;
  // actual population-level intercept
  real b_sigmabias_Intercept = Intercept_sigmabias;
  // actual population-level intercept
  real b_sigmandt_Intercept = Intercept_sigmandt;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

