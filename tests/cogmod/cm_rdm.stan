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

// ---------------------------------------------------------------------------
// Two-accumulator Racing Diffusion Model (Tillman, Van Zandt & Logan, 2020).
//
// Two Wald accumulators race to a common threshold b = boundary + sigmabias, each
// starting from z ~ Uniform(0, sigmabias). The observed trial contributes the
// winner's density times the loser's survival.
//
// Everything is carried in log space. That is not stylistic: for moderate
// drift the survival underflows to exactly zero (at drift 6 it does so by
// t = 4), and `log(0)` hands the sampler a zero gradient, which stalls it
// silently rather than erroring. The same applies to the density, whose
// individual terms underflow long before the density itself does.
//
// This is the most expensive likelihood in the package, so two things are
// deliberate and worth keeping when editing it. The normal CDF dominates the
// cost, and each one is evaluated exactly once: `log_g` takes log Phi(u) from
// its caller instead of recomputing Phi(u) internally. And nothing on the hot
// path returns a vector - Stan allocates on the autodiff stack for that, and
// here it would be per term, per observation, per leapfrog step. Together the
// two are worth about 1.5x on the gradient, measured against the term-by-term
// signed-accumulator form this replaced.
// ---------------------------------------------------------------------------

// Below this ratio of start-point range to sqrt(t), the threshold difference
// quotient cancels; the midpoint (plain Wald) limit is then both cheaper and
// more accurate, being second-order in the range.
// Below RDM_EPS_V the survival's 1 / (2 * drift) factor takes its driftless
// limit instead.

// log(u * Phi(u) + phi(u)) = log of the antiderivative of Phi, given
// lPhi = log Phi(u) that the caller already has.
//
// Taking lPhi as an argument rather than computing Phi(u) here is what keeps
// the survival down to four normal-CDF evaluations instead of six: the caller
// needs log Phi(alpha) and log Phi(beta) for its own terms anyway, and the
// normal CDF is by far the most expensive thing in this file.
real cogmod_rdm_log_g_lphi(real u, real lPhi) {
  // For u < -10 both terms underflow *and* they cancel to order u^-2, so factor
  // out phi(u) and expand 1 - |u| * MillsRatio(|u|) asymptotically. lPhi is not
  // touched on this branch.
  if (u <= -10) {
    real x2 = square(u);
    real s = 0;
    real term = 1;
    for (j in 1:12) {
      term *= (2.0 * j - 1) / x2;
      s += (j % 2 == 1) ? term : -term;
    }
    return -0.5 * x2 - 0.9189385332046727 + log(s);
  }
  real lphi = -0.5 * square(u) - 0.9189385332046727;
  // g(u) > 0 throughout. Below zero the two terms cancel, so take the
  // difference in log space rather than forming u * Phi(u) + phi(u) directly.
  if (u >= 0) return log_sum_exp(log(u) + lPhi, lphi);
  return log_diff_exp(lphi, log(-u) + lPhi);
}

// Standalone form, for the one caller that has no log Phi(u) to hand.
real cogmod_rdm_log_g(real u) {
  if (u <= -10) return cogmod_rdm_log_g_lphi(u, 0);   // lPhi unused there
  return cogmod_rdm_log_g_lphi(u, cogmod_log_Phi(u));
}

// log(Phi(b) - Phi(a)) for b >= a, using whichever tail keeps both arguments
// away from a saturating normal CDF.
//
// The upper tail is written as cogmod_log_Phi(-a) rather than as a Stan
// upper-tail function: std_normal_lccdf() collapses to -inf once its argument
// passes about 8.3 (and is already wrong in the 3rd decimal at 8), and the
// distinction is not academic here -- alpha reaches 10 for a reaction time only
// a few milliseconds above the non-decision time. cogmod_log_Phi() of the
// negated argument is the same quantity, finite and differentiable however far
// out.
real cogmod_rdm_log_diff_Phi(real a, real b) {
  if (b <= a) return negative_infinity();
  if (a >= 0) return log_diff_exp(cogmod_log_Phi(-a), cogmod_log_Phi(-b));
  if (b <= 0) return log_diff_exp(cogmod_log_Phi(b), cogmod_log_Phi(a));
  return log(Phi(b) - Phi(a));
}

// log density of the first passage time t (net of non-decision time) for a
// diffusion with drift nu, threshold offset k, start-point range A.
real cogmod_rdm_wald_ldens(real t, real nu, real k, real A) {
  if (t <= 0) return negative_infinity();
  real st = sqrt(t);

  if (A / st < 1e-4) {              // midpoint plain Wald
    real bm = k + 0.5 * A;
    return log(bm) - 0.5 * (1.8378770664093453 + 3 * log(t))
           - square(bm - nu * t) / (2 * t);
  }

  real alpha = (k - nu * t) / st;
  real beta  = (k + A - nu * t) / st;

  // T1 = drift * (Phi(beta) - Phi(alpha))
  real l1 = log(abs(nu)) + cogmod_rdm_log_diff_Phi(alpha, beta);
  real s1 = nu > 0 ? 1 : (nu < 0 ? -1 : 0);

  // T2 = (phi(alpha) - phi(beta)) / sqrt(t); phi is even, so the closer
  // argument to zero wins and decides the sign.
  real lpa = -0.5 * square(alpha) - 0.9189385332046727;
  real lpb = -0.5 * square(beta)  - 0.9189385332046727;
  real s2; real l2;
  if (abs(alpha) <= abs(beta)) {
    s2 = 1;  l2 = log_diff_exp(lpa, lpb) - log(st);
  } else {
    s2 = -1; l2 = log_diff_exp(lpb, lpa) - log(st);
  }

  // The density is positive, so the two terms add when they agree in sign and
  // otherwise reduce to (larger - smaller). Deciding that here rather than
  // through the signed accumulator keeps everything scalar: a vector-returning
  // helper allocates on Stan's autodiff stack on every call, on every leapfrog
  // step, for every observation. A zero drift leaves l1 at -inf and falls
  // through the same branch.
  real lnum;
  if (s1 == s2) {
    lnum = log_sum_exp(l1, l2);
  } else if (l1 > l2) {
    lnum = log_diff_exp(l1, l2);
  } else if (l2 > l1) {
    lnum = log_diff_exp(l2, l1);
  } else {
    lnum = negative_infinity();
  }
  return lnum - log(A);
}

// log survival of the first passage time t. Computed from the closed-form
// antiderivative of the Wald survival in the threshold, NOT as log(1 - CDF):
// the latter underflows to -inf at ordinary parameter values.
//
// Writing G for that antiderivative, S * A = G(k + A) - G(k), and G splits into
// two pieces that are each monotone in the threshold. Grouping the six terms
// into those two differences - rather than accumulating them one at a time with
// signs - makes each group's sign known in advance, which removes every signed
// accumulator from the hot path and, as a side effect, cancels less: measured
// against the R implementation the grouped form is accurate to 6e-11 where the
// term-by-term one reached 5e-9. (The second group is itself assembled from
// three signed pieces, for the reason given at D2 below, but with its sign
// still known in advance.)
real cogmod_rdm_wald_lsurv(real t, real nu, real k, real A) {
  if (t <= 0) return 0;             // log(1): nothing finishes before the ndt
  real st = sqrt(t);
  real b  = k + A;

  if (A / st < 1e-4) {              // midpoint plain Wald survival
    real bm = k + 0.5 * A;
    real m1 = cogmod_log_Phi((bm - nu * t) / st);
    real m2 = 2 * nu * bm + cogmod_log_Phi(-(bm + nu * t) / st);
    return m1 > m2 ? log_diff_exp(m1, m2) : negative_infinity();
  }

  real alpha = (k - nu * t) / st;
  real beta  = (b - nu * t) / st;

  if (abs(nu) < 1e-7) {             // driftless limit
    // S = 1 - (2 sqrt(t) / A) * (g(-k/st) - g(-b/st)); already carries its 1/A.
    real lsub = 0.6931471805599453 + log(st) - log(A)
                + log_diff_exp(cogmod_rdm_log_g(-k / st),
                               cogmod_rdm_log_g(-b / st));
    return lsub < 0 ? log1m_exp(lsub) : negative_infinity();
  }

  real linv = -log(2 * abs(nu));

  // The two shared normal CDFs: log_g needs them, and so does D2 below.
  real lPa = cogmod_log_Phi(alpha);
  real lPb = cogmod_log_Phi(beta);

  // S * A = D1 - D2, both pieces positive.
  //
  // D1 = sqrt(t) * (g(beta) - g(alpha)), positive because g is increasing.
  real lD1 = 0.5 * log(t) + log_diff_exp(cogmod_rdm_log_g_lphi(beta, lPb),
                                         cogmod_rdm_log_g_lphi(alpha, lPa));
  // D2 = |R(k + A) - R(k)| / (2 |nu|), for
  //     R(x) = exp(2 nu x) Phi(-(x + nu t) / st) + Phi((x - nu t) / st)
  //          = E(x) + Phi(alpha_x),
  // which increases with the threshold when nu > 0 and decreases when nu < 0
  // (dR/dx = 2 nu E(x): the other two derivative terms cancel by the Wald
  // reflection identity exp(2 nu x) phi((x + nu t) / st) = phi((x - nu t) / st)).
  // Dividing by 2 nu flips the second case back, so D2 is positive either way.
  //
  // It is NOT formed as log_diff_exp(log R(b), log R(k)). Just above the
  // non-decision time alpha and beta run past 37, Phi(alpha) and Phi(beta) both
  // round to exactly 1 and the E terms underflow next to them, so the two logs
  // are exactly 0 and log_diff_exp(0, 0) is evaluated. Its value, -inf, is
  // harmless - D2 really is negligible there - but its reverse-mode adjoint is
  // 0 / expm1(0) = 0 / 0, and that NaN propagates to the gradient of every
  // parameter. Stan treats a NaN gradient as a divergent transition, and with
  // `ndt` sitting a few milliseconds below the fastest responses the sampler
  // crossed that half-millisecond window under most trajectories: 60% of
  // transitions divergent on the lexical decision data of the decision-making
  // article, with the posterior itself perfectly healthy.
  //
  // So the difference is taken term by term instead:
  //     R(b) - R(k) = [Phi(beta) - Phi(alpha)] + E(b) - E(k) = P + E_b - E_k,
  // every piece of which is a log of something small and stays away from the
  // saturated end of the normal CDF. P comes from whichever tail is small; the
  // lower tail reuses lPa and lPb, which are exact there, and the upper one
  // costs two more normal CDFs but only where alpha is large. E_b and E_k stay
  // single exponents so that they survive the range where exp(2 nu x) and
  // Phi(w) separately overflow and underflow. The sign of E_b - E_k is
  // genuinely either (E alone is not monotone in the threshold), so it is
  // resolved explicitly; the sum is positive for nu > 0 and negative for
  // nu < 0. The guards below only ever fire when the difference is under one
  // ulp of its terms, where D2 / D1 is far below double precision anyway.
  // Measured against quadrature this form is as accurate as the grouped one;
  // what it buys is a gradient that stays finite as t -> 0.
  real lEb = 2 * nu * b + cogmod_log_Phi(-(b + nu * t) / st);
  real lEk = 2 * nu * k + cogmod_log_Phi(-(k + nu * t) / st);
  real lP = alpha < 3
            ? log_diff_exp(lPb, lPa)
            : log_diff_exp(cogmod_log_Phi(-alpha), cogmod_log_Phi(-beta));
  real lD2;
  if (nu > 0) {
    if (lEb >= lEk) {
      lD2 = log_sum_exp(lP, lEb > lEk ? log_diff_exp(lEb, lEk) : negative_infinity());
    } else {
      real lx = log_sum_exp(lP, lEb);
      lD2 = lx > lEk ? log_diff_exp(lx, lEk) : negative_infinity();
    }
  } else {
    real lx = log_sum_exp(lP, lEb);
    lD2 = lEk > lx ? log_diff_exp(lEk, lx) : negative_infinity();
  }
  lD2 += linv;

  real ls = lD1 > lD2 ? log_diff_exp(lD1, lD2) : negative_infinity();
  return fmin(ls - log(A), 0);
}

// Log-likelihood for one observation from the shifted Racing Diffusion model.
// Y: observed reaction time.
// dec: the observed choice, 0 or 1.
// mu: drift rate of accumulator 0 (>= 0).
// driftone: drift rate of accumulator 1 (>= 0).
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
// The decision component is carried entirely in log space. That is not
// stylistic: for moderate drift the loser's survival underflows to exactly
// zero (at drift 6 it does so by t = 4), and log(0) hands the sampler a
// zero gradient, which stalls it silently rather than erroring.
real cogmod_rdm_lpdf(real Y, real mu, real driftone, real sigmabias, real boundary, real ndt, real poutlier, int dec) {
    // Parameter checks
    if (boundary <= 0 || sigmabias < 0 || mu < 0 || driftone < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (dec < 0 || dec > 1) return negative_infinity();
    if (Y <= 0) return negative_infinity();

    real lp_out = 0.6904993792294275 - 12.5 * square(Y);
    real t_adj  = Y - ndt;

    // Faster than the non-decision time: only the outlier component can have
    // produced this response.
    if (t_adj <= 0) return log(poutlier) + lp_out;

    real lp_dec = cogmod_rdm_wald_ldens(t_adj, dec == 0 ? mu : driftone, boundary, sigmabias)
      + cogmod_rdm_wald_lsurv(t_adj, dec == 0 ? driftone : mu, boundary, sigmabias);
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
  real Intercept_sigmabias;  // temporary intercept for centered predictors
  real Intercept_boundary;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_driftone | 3, 2);
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
    vector[N] sigmabias = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] boundary = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    driftone += Intercept_driftone;
    sigmabias += Intercept_sigmabias;
    boundary += Intercept_boundary;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    mu = log1p_exp(mu);
    driftone = log1p_exp(driftone);
    sigmabias = log1p_exp(sigmabias);
    boundary = log1p_exp(boundary);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_rdm_lpdf(Y[n] | mu[n], driftone[n], sigmabias[n], boundary[n], ndt[n], poutlier[n], dec[n]);
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
  real b_sigmabias_Intercept = Intercept_sigmabias;
  // actual population-level intercept
  real b_boundary_Intercept = Intercept_boundary;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

