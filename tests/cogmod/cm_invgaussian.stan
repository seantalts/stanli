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

// Log density of the fixed-drift Wald with drift v > 0 and threshold a > 0,
// at t > 0: an inverse Gaussian with mean a / v and shape a^2.
real cogmod_wald_ldens(real t, real v, real a) {
  return log(a) - 0.5 * (log(2 * pi()) + 3 * log(t)) - square(a - v * t) / (2 * t);
}

// Log CDF of the same Wald. The exp(2 a v) factor overflows on its own long
// before the product it belongs to stops being representable, so it is folded
// into the exponent.
real cogmod_wald_logcdf(real t, real v, real a) {
  if (t <= 0) return negative_infinity();
  real st = sqrt(t);
  return log_sum_exp(
    cogmod_log_Phi((v * t - a) / st),
    2 * a * v + cogmod_log_Phi(-(v * t + a) / st)
  );
}

// Log survival of the same Wald: the DIFFERENCE of the two terms,
//   S(t) = Phi((a - v t) / sqrt(t)) - exp(2 a v) Phi(-(a + v t) / sqrt(t)),
// rather than log1m_exp(logcdf), which has lost every digit by the time a slow
// error is censored. Same form as cogmod_rdm_wald_lsurv() at A = 0.
real cogmod_wald_lsurv(real t, real v, real a) {
  if (t <= 0) return 0;
  real st = sqrt(t);
  real m1 = cogmod_log_Phi((a - v * t) / st);
  real m2 = 2 * a * v + cogmod_log_Phi(-(a + v * t) / st);
  return m1 > m2 ? log_diff_exp(m1, m2) : negative_infinity();
}

// log INT_0^t F(s) ds, the integrated CDF, at t > 0:
//   I(t) = t F(t) - (a / v) [Phi(alpha) - exp(2 a v) Phi(beta)],
// alpha = (v t - a) / sqrt(t), beta = -(v t + a) / sqrt(t). The bracket is
// (v / a) E[T; T <= t] and t F(t) exceeds its multiple, so both differences
// are taken in log space and are positive.
real cogmod_wald_liF(real t, real v, real a) {
  real st = sqrt(t);
  real lPa = cogmod_log_Phi((v * t - a) / st);
  real lPb = 2 * a * v + cogmod_log_Phi(-(v * t + a) / st);
  real lF = log_sum_exp(lPa, lPb);
  real lP = lPa > lPb ? log_diff_exp(lPa, lPb) : negative_infinity();
  real x = log(t) + lF;
  real y = log(a / v) + lP;
  return x > y ? log_diff_exp(x, y) : negative_infinity();
}

// log INT_t^Inf S(s) ds, the integrated survival:
//   R(t) = (a / v - t) Phi(-alpha) + (a / v + t) exp(2 a v) Phi(beta),
// which is a / v - t below the shift (S = 1 there). Above the mean the first
// coefficient is negative and R is a difference, positive because it is the
// integral of a survival.
real cogmod_wald_liS(real t, real v, real a) {
  if (t <= 0) return log(a / v - t);
  real st = sqrt(t);
  real lQa = cogmod_log_Phi(-(v * t - a) / st);
  real lQb = 2 * a * v + cogmod_log_Phi(-(v * t + a) / st);
  real c1 = a / v - t;
  real c2 = a / v + t;
  if (c1 >= 0) return log_sum_exp(log(c1) + lQa, log(c2) + lQb);
  real x = log(c2) + lQb;
  real y = log(-c1) + lQa;
  return x > y ? log_diff_exp(x, y) : negative_infinity();
}

// The fixed-drift Wald with its shift smeared over Uniform(0, st0): the log
// density (what = 0), log CDF (1) or log survival (2) of the decision
// component seen at t = Y - ndt. Each is a difference quotient of the
// functions above,
//   f = [F(t) - F(t - st0)] / st0 = [S(t - st0) - S(t)] / st0
//   G = [I(t) - I(t - st0)] / st0,   1 - G = [R(t - st0) - R(t)] / st0,
// taken between whichever pair is small - the CDFs near the shift, the
// survivals in the tail, decided by the midpoint's CDF - and reduced to the
// one-sided form when the interval reaches back past the shift. Below
// st0 / t = 1e-5 the quotient has lost five digits and the midpoint value,
// with error O(st0^2), is the better answer. Same branches, same order, as
// .lwald_st0_fixed() in R.
real cogmod_wald_st0(real t, real v, real a, real st0, int what) {
  if (st0 <= 0 || st0 < 1e-5 * t) {
    real m = st0 <= 0 ? t : t - 0.5 * st0;
    if (what == 0) return cogmod_wald_ldens(m, v, a);
    if (what == 1) return cogmod_wald_logcdf(m, v, a);
    return cogmod_wald_lsurv(m, v, a);
  }
  real t0 = t - st0;
  real ls = log(st0);
  if (what == 0) {
    if (t0 <= 0) return cogmod_wald_logcdf(t, v, a) - ls;
    if (cogmod_wald_logcdf(t - 0.5 * st0, v, a) < -0.69314718055994529) {
      real x = cogmod_wald_logcdf(t, v, a);
      real y = cogmod_wald_logcdf(t0, v, a);
      return x > y ? log_diff_exp(x, y) - ls : negative_infinity();
    }
    real x = cogmod_wald_lsurv(t0, v, a);
    real y = cogmod_wald_lsurv(t, v, a);
    return x > y ? log_diff_exp(x, y) - ls : negative_infinity();
  }
  if (what == 1) {
    real x = cogmod_wald_liF(t, v, a);
    if (t0 <= 0) return x - ls;
    real y = cogmod_wald_liF(t0, v, a);
    return x > y ? log_diff_exp(x, y) - ls : negative_infinity();
  }
  real x = cogmod_wald_liS(t0, v, a);
  real y = cogmod_wald_liS(t, v, a);
  return x > y ? log_diff_exp(x, y) - ls : negative_infinity();
}

// Any of the three above, marginalised over a drift ~ Normal(mu, sigmadrift)
// truncated at zero, by 64-point Gauss-Legendre quadrature on the interval
// carrying the truncated normal's mass. Assembled with log_sum_exp so the
// survival keeps its digits where every term is tiny.
real cogmod_wald_sv_lquad(real t, real mu, real boundary, real sigmadrift,
                          real st0, int what) {
  vector[64] gx = [-0.99930504173577184, -0.99634011677195478, -0.99101337147674351, -0.98333625388462553, -0.97332682778991098, -0.96100879965205266, -0.94641137485840288, -0.92956917213194012, -0.91052213707850305, -0.88931544599511381, -0.86599939815409277, -0.84062929625258032, -0.81326531512279732, -0.78397235894334139, -0.7528199072605315, -0.71988185017161066, -0.68523631305423294, -0.64896547125465709, -0.61115535517239361, -0.57189564620263411, -0.53127946401989412, -0.4894031457070529, -0.44636601725346403, -0.40227015796399135, -0.35722015833766774, -0.31132287199021103, -0.26468716220876742, -0.21742364374000722, -0.16964442042399264, -0.12146281929612068, -0.072993121787799042, -0.0243502926634247, 0.024350292663424367, 0.072993121787799264, 0.12146281929612057, 0.16964442042399253, 0.21742364374000711, 0.26468716220876753, 0.31132287199021114, 0.35722015833766807, 0.40227015796399168, 0.44636601725346403, 0.48940314570705301, 0.53127946401989468, 0.57189564620263411, 0.61115535517239317, 0.64896547125465731, 0.68523631305423316, 0.71988185017161088, 0.75281990726053194, 0.7839723589433415, 0.81326531512279754, 0.84062929625258043, 0.86599939815409288, 0.88931544599511403, 0.91052213707850282, 0.92956917213193957, 0.94641137485840277, 0.96100879965205377, 0.97332682778991098, 0.98333625388462598, 0.99101337147674429, 0.99634011677195533, 0.99930504173577217]';
  vector[64] lgw = [-6.3293005090299017, -5.4853620773590359, -5.0352674957809302, -4.7277040083159543, -4.4946902455642821, -4.307806539335628, -4.1524379474758284, -4.0200695822260988, -3.905304467755339, -3.8045069931459321, -3.7151124471642243, -3.6352450530842337, -3.5634925968589939, -3.4987664273949801, -3.44021075164116, -3.3871417710780261, -3.3390056141482569, -3.2953485146807311, -3.2557952054757089, -3.2200329657796014, -3.187799649113956, -3.1588745708662005, -3.1330714889292883, -3.1102331427320218, -3.0902269715508091, -3.0729417393885887, -3.0582848678711168, -3.0461803312583249, -3.0365670057291503, -3.029397393151521, -3.0246366606768751, -3.0222619538330395, -3.0222619538330382, -3.0246366606768609, -3.0293973931515272, -3.0365670057291605, -3.0461803312583005, -3.058284867871127, -3.0729417393885705, -3.0902269715508268, -3.110233142732008, -3.1330714889292772, -3.1588745708662009, -3.1877996491139386, -3.2200329657795868, -3.2557952054757311, -3.2953485146807591, -3.3390056141482893, -3.3871417710780376, -3.4402107516411529, -3.4987664273949548, -3.5634925968590361, -3.6352450530842253, -3.7151124471642052, -3.8045069931459543, -3.9053044677552706, -4.0200695822260979, -4.1524379474759314, -4.3078065393356795, -4.4946902455641755, -4.727704008315885, -5.0352674957809809, -5.4853620773588121, -6.3293005090299967]';
  real lo = fmax(mu - 10 * sigmadrift, 0);
  real hi = mu + 10 * sigmadrift;
  real half = 0.5 * (hi - lo);
  real mid = 0.5 * (hi + lo);
  real lnorm = cogmod_log_Phi(mu / sigmadrift);
  vector[64] terms;
  for (j in 1:64) {
    real v = mid + half * gx[j];
    real lw = lgw[j] + log(half) + normal_lpdf(v | mu, sigmadrift) - lnorm;
    terms[j] = lw + cogmod_wald_st0(t, v, boundary, st0, what);
  }
  return log_sum_exp(terms);
}

// Log density of the Wald decision time (no shift), with the drift rate drawn
// once per trial from Normal(mu, sigmadrift) truncated at zero and the
// non-decision time spread over a range sigmandt above ndt. sigmadrift = 0 and
// sigmandt = 0 is the plain Wald: an inverse Gaussian with mean boundary / mu
// and shape boundary^2. With sigmandt = 0 the drift is marginalised in closed
// form - a Gaussian integral - which is the branch a fit usually runs in.
real cogmod_invgaussian_decision_lpdf(real t, real mu, real boundary,
                                      real sigmadrift, real sigmandt) {
  if (sigmandt <= 0) {
    real base = log(boundary) - 0.5 * (log(2 * pi()) + 3 * log(t));
    if (sigmadrift <= 0) {
      return base - square(boundary - mu * t) / (2 * t);
    }
    real s2 = square(sigmadrift);
    real D = 1 + s2 * t;
    return base - 0.5 * log(D) - square(boundary - mu * t) / (2 * t * D)
      + cogmod_log_Phi((boundary * s2 + mu) / (sigmadrift * sqrt(D)))
      - cogmod_log_Phi(mu / sigmadrift);
  }
  if (sigmadrift <= 0) return cogmod_wald_st0(t, mu, boundary, sigmandt, 0);
  return cogmod_wald_sv_lquad(t, mu, boundary, sigmadrift, sigmandt, 0);
}

// Log CDF and log survival of the Wald decision time (no shift), the
// counterparts of cogmod_invgaussian_decision_lpdf().
real cogmod_invgaussian_decision_lcdf(real t, real mu, real boundary,
                                      real sigmadrift, real sigmandt) {
  if (sigmadrift <= 0) return cogmod_wald_st0(t, mu, boundary, sigmandt, 1);
  return cogmod_wald_sv_lquad(t, mu, boundary, sigmadrift, sigmandt, 1);
}

real cogmod_invgaussian_decision_lccdf(real t, real mu, real boundary,
                                       real sigmadrift, real sigmandt) {
  if (sigmadrift <= 0) return cogmod_wald_st0(t, mu, boundary, sigmandt, 2);
  return cogmod_wald_sv_lquad(t, mu, boundary, sigmadrift, sigmandt, 2);
}

// Log-likelihood for one observation from the shifted Wald (inverse Gaussian) model.
// Y: observed reaction time.
// mu: drift rate, the average speed of evidence accumulation (> 0).
// boundary: decision threshold, the evidence needed to respond (> 0).
// sigmadrift: between-trial SD of the drift rate (>= 0), which is drawn from a
//    Normal(mu, sigmadrift) truncated at zero. 0 is the plain Wald.
// sigmandt: between-trial range of the non-decision time (st0, >= 0), in the same
//    unit as Y, with `ndt` its lower bound. 0 is a fixed non-decision time.
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
// sigmadrift and poutlier both fatten the right tail, and they are only weakly
// distinguishable: on 2000 simulated trials at mu = 3, boundary = 0.5, sigmadrift =
// 0.8, estimating sigmadrift buys about 2 log-likelihood units over fixing it at
// zero. Fix it (`sigmadrift = 0` in bf()) unless the design can identify it.
// sigmandt is harder still: it shares the leading edge with ndt and poutlier, and
// should be fixed (`sigmandt = 0` in bf()) unless there is a lot of data, a strong
// prior, or both.
real cogmod_invgaussian_lpdf(real Y, real mu, real boundary, real sigmadrift, real sigmandt, real ndt, real poutlier) {
    // Parameter checks
    if (mu <= 0 || boundary <= 0 || sigmadrift < 0 || sigmandt < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
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

    return log_mix(poutlier, lp_out, cogmod_invgaussian_decision_lpdf(t_adj | mu, boundary, sigmadrift, sigmandt));
}

// Log CDF and log survival of the same mixture, for brms's cens() addition
// term. See ?rcogmod_invgaussian for what censoring a reaction time means and
// when it is the right model.
real cogmod_invgaussian_lcdf(real Y, real mu, real boundary, real sigmadrift, real sigmandt, real ndt, real poutlier) {
    if (mu <= 0 || boundary <= 0 || sigmadrift < 0 || sigmandt < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (Y <= 0) return negative_infinity();
    // Outlier CDF: 2 Phi(Y / s) - 1 = erf(Y / (s sqrt(2)))
    real lF_out = log(erf(Y * 3.5355339059327369));
    real t_adj  = Y - ndt;
    if (t_adj <= 0) return log(poutlier) + lF_out;
    return log_mix(poutlier, lF_out, cogmod_invgaussian_decision_lcdf(t_adj | mu, boundary, sigmadrift, sigmandt));
}

real cogmod_invgaussian_lccdf(real Y, real mu, real boundary, real sigmadrift, real sigmandt, real ndt, real poutlier) {
    if (mu <= 0 || boundary <= 0 || sigmadrift < 0 || sigmandt < 0 || ndt < 0 || poutlier < 0 || poutlier > 1) {
      return negative_infinity();
    }
    if (Y <= 0) return 0;
    // Outlier survival: 2 Phi(-Y / s), through the lower tail (see above)
    real lS_out = 0.69314718055994529 + std_normal_lcdf(-Y * 5);
    real t_adj  = Y - ndt;
    // Not yet past the non-decision time: the decision process cannot have
    // finished, so its survival is exactly 1.
    if (t_adj <= 0) return log_mix(poutlier, lS_out, 0);
    return log_mix(poutlier, lS_out, cogmod_invgaussian_decision_lccdf(t_adj | mu, boundary, sigmadrift, sigmandt));
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
  real Intercept_boundary;  // temporary intercept for centered predictors
  real Intercept_sigmadrift;  // temporary intercept for centered predictors
  real Intercept_sigmandt;  // temporary intercept for centered predictors
  real Intercept_ndt;  // temporary intercept for centered predictors
  real Intercept_poutlier;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_sigmadrift | 0, 1);
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
    vector[N] sigmadrift = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] sigmandt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] ndt = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] poutlier = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    boundary += Intercept_boundary;
    sigmadrift += Intercept_sigmadrift;
    sigmandt += Intercept_sigmandt;
    ndt += Intercept_ndt;
    poutlier += Intercept_poutlier;
    mu = log1p_exp(mu);
    boundary = log1p_exp(boundary);
    sigmadrift = log1p_exp(sigmadrift);
    sigmandt = exp(sigmandt);
    ndt = exp(ndt);
    poutlier = inv_logit(poutlier);
    for (n in 1:N) {
      target += cogmod_invgaussian_lpdf(Y[n] | mu[n], boundary[n], sigmadrift[n], sigmandt[n], ndt[n], poutlier[n]);
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
  real b_sigmadrift_Intercept = Intercept_sigmadrift;
  // actual population-level intercept
  real b_sigmandt_Intercept = Intercept_sigmandt;
  // actual population-level intercept
  real b_ndt_Intercept = Intercept_ndt;
  // actual population-level intercept
  real b_poutlier_Intercept = Intercept_poutlier;
}

