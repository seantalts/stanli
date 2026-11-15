// ML-UMR: parametric survival (time-to-event), relaxed model with
// treatment-specific prognostic coefficients (allows effect modification).
// Unanchored comparison of single-arm trials.
//
//   IPD  (index treatment)     : individual event/censoring times + covariates.
//   AgD  (comparator treatment): reconstructed pseudo-IPD whose likelihood is
//                                integrated over the comparator covariate
//                                distribution (integration points).
//
// 9 parametric distributions via integer `dist` dispatch (see
// include/survival_functions.stan).
//
// The combined (intercepts + treatment-specific covariates) design is supplied
// as `Xq_*` (raw centered design when qr = 0, scaled thin-QR factor Q when
// qr = 1); coefficients are recovered as `allbeta = R_inv * beta_tilde`. See R/mlumr.R (.mlumr_qr_design()) for the centering and QR design construction.

functions {
// Shared prior log-density dispatchers for ML-UMR Stan models.
//
// These helpers branch on an integer distribution code so that Stan data
// dictates the prior family at runtime. Call with `target += ...`.
//
//   dist == 0 : normal
//   dist == 1 : student_t (with df)
//   dist == 2 : exponential (only supported by log_prior_sigma;
//               `scale` is interpreted as 1 / rate so that the user-facing
//               `prior_exponential(rate)` rescales correctly; see priors.R)
//
// `df` is ignored when dist != 1; pass any positive placeholder (the
// Stan data block constrains it to be > 0 regardless).

real log_prior_scalar(real x, real location, real scale, int dist, real df) {
  if (dist == 0) return normal_lpdf(x | location, scale);
  else return student_t_lpdf(x | df, location, scale);
}

real log_prior_vector(vector x, vector location, vector scale, int dist, real df) {
  if (dist == 0) return normal_lpdf(x | location, scale);
  else return student_t_lpdf(x | df, location, scale);
}

real log_prior_sigma(real sigma, real location, real scale, int dist, real df) {
  if (dist == 0) return normal_lpdf(sigma | location, scale);
  else if (dist == 1) return student_t_lpdf(sigma | df, location, scale);
  else return exponential_lpdf(sigma | inv(scale));
}

// Prior on the standardized coordinate z, where a regression coefficient
// is reparameterized as `beta = location + scale .* z`. Because the
// transformation is affine in data, Stan needs no Jacobian, and the
// induced prior on `beta` matches the user-specified family:
//   z ~ std_normal()         => beta ~ normal(location, scale)
//   z ~ student_t(df, 0, 1)  => beta ~ student_t(df, location, scale)
//                               (scale invariance of Student-t).
// This affine reparameterization decouples adaptation from the prior
// scale; HMC sees a unit-scale, zero-centered coordinate, which
// improves sampling when the prior scale is mismatched with the
// posterior scale (common with the default weakly-informative priors).
// Note: intentionally retained as API surface; not called by any current model
// (none use this non-centered std-vector prior form). Kept for extension use.
real log_prior_std_vector(vector z, int dist, real df) {
  if (dist == 0) return std_normal_lpdf(z);
  else return student_t_lpdf(z | df, 0, 1);
}
// Shared survival likelihood / hazard / standardization helpers for the
// ML-UMR parametric survival models (mlumr_survival_{spfa,relaxed}.stan).
//
// Distribution codes (integer `dist`, multinma-style dispatch):
//   1 = Exponential (PH)   4 = Exponential (AFT)   7 = Log-logistic (AFT)
//   2 = Weibull (PH)       5 = Weibull (AFT)        8 = Gamma (AFT)
//   3 = Gompertz (PH)      6 = Log-normal (AFT)     9 = Generalized Gamma (AFT)
//
// `aux` is the (positive) shape/scale parameter where one exists; `aux2` is the
// second shape for the generalized gamma. For exponential families they are
// unused placeholders (value 1).
//
// Censoring status codes (Surv-derived; see R/survival.R .get_surv_data()):
//   0 = right-censored   1 = event   2 = left-censored   3 = interval-censored
// For interval censoring `time` is the upper bound and `start_time` the lower
// bound. `delay_time > 0` flags left truncation (delayed entry).

// The largest logarithm whose exponential a double can hold: exp() of
// anything above this is infinite. Branches that ask whether an exponential
// overflows must test the logarithm against this rather than form the
// exponential and call is_inf() on it. An exponential built only to be asked
// that question still goes on the autodiff tape, and the reverse sweep visits
// every node it holds: exp()'s callback adds `adjoint * value` to its input's
// adjoint, and an unused node's adjoint is zero, so 0 * inf is NaN and the
// NaN reaches the gradient of a log density whose value came back finite.
// A Gamma of shape 1 at eta = -710 over (1, 1 + 2^-52] returned a log
// likelihood of -4.96e292 with a gradient of NaN for exactly that reason.
real log_double_max() {
  return 709.782712893384;
}

// log of the regularized upper incomplete gamma Q(k, x), evaluated entirely on
// the log scale by Legendre's continued fraction:
//
//   Q(k, x) = exp(-x + k log x - lgamma(k)) * CF(k, x),
//   CF(k, x) = 1 / (x+1-k - 1(1-k)/(x+3-k - 2(2-k)/(x+5-k - ...))),
//
// where CF is an O(1/x) quantity. It is the exponential prefactor that
// underflows, not the continued fraction, so keeping the prefactor as a log
// leaves the result finite wherever log Q(k, x) is itself representable.
//
// Used only beyond k + max(1, sqrt(k)), where the fraction is safely in its
// tail regime. Closer to a large k, the native incomplete-gamma function is
// more reliable.
real log_gamma_q_cf_factor(real k, real x) {
  real tiny = 1e-300;
  real b = x + 1 - k;
  real c = inv(tiny);
  real d = inv(b);
  real h = d;
  for (i in 1 : 300) {
    real an = -i * (i - k);
    real del;
    b += 2;
    d = an * d + b;
    if (abs(d) < tiny) {
      d = tiny;
    }
    c = b + an / c;
    if (abs(c) < tiny) {
      c = tiny;
    }
    d = inv(d);
    del = d * c;
    h *= del;
    if (abs(del - 1) < 1e-14) {
      break;
    }
  }
  return log(h);
}

real log_gamma_q_cf(real k, real x) {
  return -x + k * log(x) - lgamma(k) + log_gamma_q_cf_factor(k, x);
}

// Log of the regularized lower incomplete gamma P(k, x), using its convergent
// series without first forming x. This keeps left-tail probabilities finite
// when log(x) is representable but x itself underflows to zero.
real log_gamma_p_series(real k, real log_x) {
  // Terms are accumulated relative to the first, whose 1/k is folded into
  // lgamma(k + 1) at the end. Carrying the -log(k) inside the sum instead and
  // finishing with -lgamma(k) forms a difference of two quantities that both
  // grow like -log(k) as the shape shrinks: at k = 1e-18 each is 41.4465, the
  // rounding in their difference is about 9.2e-15, and the answer being sought
  // is k * log_x = -1e-15. The difference therefore came out as exactly 0, and
  // the caller read that as a survival of zero, putting an artificial
  // zero-likelihood wall in a region the sampler can reach: aux2 is declared
  // only `<lower=0>`. This form has no such subtraction and agrees with the R
  // helper, which uses lgamma(k + 1) for the same reason.
  real log_term = 0;
  real log_total = 0;
  for (i in 1 : 300) {
    log_term += log_x - log(k + i);
    log_total = log_sum_exp(log_total, log_term);
    if (exp(log_term - log_total) < 1e-14) {
      break;
    }
  }
  return -exp(log_x) + k * log_x - lgamma(k + 1) + log_total;
}

// Generalized Gamma log density (Lawless parameterization).
real gengamma_lpdf(real y, real mu, real sigma, real k) {
  real Q = pow(k, -0.5);
  real z = Q * (log(y) - mu) / sigma;
  real log_w = log(k) + z;
  return -log(sigma) - log(y) - 0.5 * log(k) * (1 - 2 * k)
         + k * z - exp(log_w) - lgamma(k);
}

// Log upper standard-normal tail, log Phi(-z).
//
// Written through the symmetry Phi(-z) = 1 - Phi(z) rather than by calling
// std_normal_lccdf() directly, because Stan's two normal tail functions are not
// equally well conditioned: std_normal_lccdf(z) already carries a relative error
// of 5e-4 at z = 8 and UNDERFLOWS TO -inf at z = 8.5, where the true value is a
// perfectly representable -39.2, while std_normal_lcdf() is exact to 2.5e-16 out
// to z = -300. Using the accurate one on the reflected argument therefore costs
// nothing and removes a band in which the log-normal likelihood was silently
// truncated: a right-censored observation whose (log t - eta) / sigma landed
// above 8.5 contributed -inf and the proposal was rejected, and the marginal
// hazard generated quantities became inf or NaN. Verified against the reference
// normal tail over z in [1, 300]: maximum relative error 2.7e-16.
real log_std_normal_surv(real z) {
  return std_normal_lcdf(-z);
}

// Log inverse Mills ratio log[phi(z) / Phi(-z)], the standard-normal hazard.
// The infinite-z limit is taken explicitly: both terms are -inf there and their
// difference would be NaN, whereas the hazard diverges. An infinite z is
// reachable whenever a tiny scale parameter divides the standardized time.
real log_std_normal_hazard(real z) {
  if (is_inf(z) && z > 0) return positive_infinity();
  // Above z = 20, the asymptotic series for the inverse Mills ratio, which is
  // the same one the R helper uses so the two agree exactly. The difference
  // below subtracts two quantities that both grow like z^2/2, and that
  // cancellation is what a large z destroys: at z = 1e4 the difference is
  // already wrong in the tenth digit, at 1e6 in the sixth, and at 1e8 it
  // returns 18 where the true value is 18.420680743952, because 5e15 has no
  // representable neighbors 18.42 apart. Checked against the continued
  // fraction z + 1/(z + 2/(z + 3/(z + ...))), which the series matches to
  // every printed digit from z = 1e3 upward. A tiny scale parameter dividing
  // the standardized time is what makes z that large, and the declaration
  // permits it.
  if (z > 20) {
    real iz2 = 1 / square(z);
    return log(z) + log1p(iz2 * (1 + iz2 * (-2 + iz2 * (10 - 74 * iz2))));
  }
  return std_normal_lpdf(z) - log_std_normal_surv(z);
}

real log_gamma_cdf_from_log_x(real k, real log_x) {
  if (log_x > log_double_max()) return 0;
  real x = exp(log_x);
  if (x <= k + fmax(1, sqrt(k))) {
    real p = gamma_p(k, x);
    if (p > 0) return log(p);
    return log_gamma_p_series(k, log_x);
  }
  return log1m_exp(log_gamma_q_cf(k, x));
}

real log_gamma_surv_from_log_x(real k, real log_x) {
  if (log_x > log_double_max()) return negative_infinity();
  real x = exp(log_x);
  // `exp(log_x)` underflows to zero below about -745, and `gamma_q(k, 0)` is
  // exactly 1, so the survival comes back as certain and a censored
  // observation contributes nothing. That is wrong whenever the shape is
  // small, because the survival depends on `x^k`, which is `exp(k * log_x)`
  // and stays of order one however far `log_x` has gone: at `k = 1e-6` and
  // `log_x = -1013.8` the survival is 0.0010127, not 1. The series reads
  // `log_x` directly and never forms `x`, so it is unaffected. A genuine
  // `log_x` of negative infinity, which is `t = 0`, still means survival 1.
  if (x == 0 && !is_inf(log_x)) {
    real log_p = log_gamma_p_series(k, log_x);
    if (log_p >= 0) return negative_infinity();
    return log1m_exp(log_p);
  }
  if (x > k + fmax(1, sqrt(k))) return log_gamma_q_cf(k, x);
  return log(gamma_q(k, x));
}

// Log survival function log S(t | eta) for a single observation.
real log_surv_scalar(int dist, real t, real eta, real aux, real aux2) {
  if (dist == 1) return -exp(log(t) + eta);                         // Exp PH
  else if (dist == 2) return -exp(aux * log(t) + eta);             // Weibull PH
  else if (dist == 3) return -exp(eta - log(aux) + aux * t + log1m_exp(-aux * t)); // Gompertz PH
  else if (dist == 4) return -exp(log(t) - eta);                    // Exp AFT
  else if (dist == 5) return -exp(aux * (log(t) - eta));           // Weibull AFT
  else if (dist == 6) return log_std_normal_surv((log(t) - eta) / aux); // Log-normal
  else if (dist == 7) return -log1p_exp(aux * (log(t) - eta));     // Log-logistic
  else if (dist == 8) return log_gamma_surv_from_log_x(aux, log(t) - eta); // Gamma
  else {                                                            // Gen. Gamma
    real Q = inv(sqrt(aux2));
    // Form the incomplete-gamma argument in LOG space. Building `w` directly
    // overflows to +inf whenever Q * (log t - eta) / aux is large, which happens
    // routinely at random inits (small `aux` divides the exponent) and killed
    // whole chains with
    //   "boost::math::tgamma: Series evaluation exceeded 1000000 iterations".
    real log_w = Q * (log(t) - eta) / aux + log(aux2);
    return log_gamma_surv_from_log_x(aux2, log_w);
  }
}

// Closed-form log hazard log h(t | eta); returns 0 for dists handled via lpdf
// (6, 8, 9); callers needing those use log_haz_full().
real log_haz_scalar(int dist, real t, real eta, real aux, real aux2) {
  if (dist == 1) return eta;                                        // Exp PH
  else if (dist == 2) return log(aux) + (aux * log(t) + eta) - log(t); // Weibull PH
  else if (dist == 3) return eta + aux * t;                         // Gompertz PH
  else if (dist == 4) return -eta;                                  // Exp AFT
  else if (dist == 5) return log(aux) + aux * (log(t) - eta) - log(t); // Weibull AFT
  else if (dist == 7) return log(aux) - log(t)
                              - log1p_exp(-aux * (log(t) - eta));   // Log-logistic
  else return 0;                                                    // 6, 8, 9 via lpdf
}

// Log hazard for all distributions (log h = log f - log S for the lpdf-based
// families), written so the tail cancellation between those two terms is taken
// analytically rather than numerically. This is live likelihood code: it is the
// event contribution under delayed entry in surv_ll_status(), and the numerator
// of the population-standardized hazard in log_mean_haz().
real log_haz_full(int dist, real t, real eta, real aux, real aux2) {
  if (dist == 6) {
    real z = (log(t) - eta) / aux;
    return log_std_normal_hazard(z) - log(aux) - log(t);
  } else if (dist == 8) {
    real log_z = log(t) - eta;
    if (log_z > log_double_max()) return -eta;
    real z = exp(log_z);
    if (z > aux + fmax(1, sqrt(aux)))
      return -log(t) - log_gamma_q_cf_factor(aux, z);
    return (aux - 1) * log_z - eta - z - lgamma(aux)
           - log_gamma_surv_from_log_x(aux, log_z);
  } else if (dist == 9) {
    real z = inv(sqrt(aux2)) * (log(t) - eta) / aux;
    real log_w = log(aux2) + z;
    if (log_w > log_double_max())
      return -log(aux) - log(t) + 0.5 * log(aux2) + z;
    real w = exp(log_w);
    if (w > aux2 + fmax(1, sqrt(aux2)))
      return -log(aux) - log(t) - 0.5 * log(aux2)
             - log_gamma_q_cf_factor(aux2, w);
    return gengamma_lpdf(t | eta, aux, aux2)
           - log_gamma_surv_from_log_x(aux2, log_w);
  }
  else
    return log_haz_scalar(dist, t, eta, aux, aux2);
}

// Log density evaluated without forming log(h) + log(S). In deep tails those
// two terms can be +inf and -inf even when the density has the well-defined
// limiting value zero. This helper is shared by the event likelihood and the
// numerator of the marginal hazard.
//
// The time arrives as a base log time plus an offset, and the centered log
// time is formed as (log_t_base - eta) + offset. The quadrature below places
// its nodes at offsets far smaller than an ULP of the base: with bounds one
// ULP apart at 0.1 and eta = log(0.1), every node's log time is eta to the
// last bit, while the standardized endpoints at a scale of 1e-17 are -17 and
// -3. Adding the offset to the small centered difference keeps that.
real log_density_offset(int dist, real log_t_base, real shift, real eta,
                        real aux, real aux2) {
  real log_t = log_t_base + shift;
  real centered = (log_t_base - eta) + shift;
  if (dist <= 5) {
    real t = exp(log_t);
    real log_ch;
    if (dist == 1) log_ch = log_t + eta;
    else if (dist == 2) log_ch = aux * log_t + eta;
    else if (dist == 3)
      log_ch = eta - log(aux) + aux * t + log1m_exp(-aux * t);
    else if (dist == 4) log_ch = centered;
    else log_ch = aux * centered;
    if (log_ch > 700) return negative_infinity();
    return log_haz_scalar(dist, t, eta, aux, aux2) - exp(log_ch);
  } else if (dist == 6) {
    real z = centered / aux;
    return -0.5 * square(z) - log(aux) - log_t - 0.5 * log(2 * pi());
  } else if (dist == 7) {
    real z = aux * centered;
    if (z >= 0)
      return log(aux) - log_t - z - 2 * log1p_exp(-z);
    return log(aux) - log_t + z - 2 * log1p_exp(z);
  } else if (dist == 8) {
    real log_z = centered;
    if (log_z > 700) return negative_infinity();
    return (aux - 1) * log_z - eta - exp(log_z) - lgamma(aux);
  } else {
    real Q = inv(sqrt(aux2));
    real z = Q * centered / aux;
    real log_w = log(aux2) + z;
    if (log_w > 700) return negative_infinity();
    return -log(aux) - log_t - 0.5 * log(aux2) * (1 - 2 * aux2)
           + aux2 * z - exp(log_w) - lgamma(aux2);
  }
}

real log_density_scalar(int dist, real t, real eta, real aux, real aux2) {
  return log_density_offset(dist, log(t), 0, eta, aux, aux2);
}

real log1m_exp_neg_exp(real log_h) {
  if (log_h < -20) {
    real h = exp(log_h);
    return log_h + log1p(-0.5 * h + square(h) / 6);
  }
  if (log_h > 700) return 0;
  return log1m_exp(-exp(log_h));
}

// Log CDF evaluated directly in the tail where 1 - S rounds to zero.
real log_cdf_scalar(int dist, real t, real eta, real aux, real aux2) {
  if (dist == 1) {
    real log_h = log(t) + eta;
    return log1m_exp_neg_exp(log_h);
  } else if (dist == 2) {
    real log_h = aux * log(t) + eta;
    return log1m_exp_neg_exp(log_h);
  } else if (dist == 3) {
    real log_h = eta - log(aux) + aux * t + log1m_exp(-aux * t);
    return log1m_exp_neg_exp(log_h);
  } else if (dist == 4) {
    real log_h = log(t) - eta;
    return log1m_exp_neg_exp(log_h);
  } else if (dist == 5) {
    real log_h = aux * (log(t) - eta);
    return log1m_exp_neg_exp(log_h);
  } else if (dist == 6) {
    // std_normal_lcdf() is the well-conditioned member of Stan's normal tail
    // pair (see log_std_normal_surv above), and the log CDF is exactly what it
    // computes, so no reflection is needed here.
    return std_normal_lcdf((log(t) - eta) / aux);
  } else if (dist == 7) {
    return -log1p_exp(-aux * (log(t) - eta));
  } else if (dist == 8) {
    return log_gamma_cdf_from_log_x(aux, log(t) - eta);
  } else {
    real log_w = log(aux2) + inv(sqrt(aux2)) * (log(t) - eta) / aux;
    return log_gamma_cdf_from_log_x(aux2, log_w);
  }
}

// Log cumulative hazard, for the five families whose H(t) has a closed form
// that survives on the log scale. Only these are ever asked for: the sole
// caller, log_cumhaz_diff(), is itself reached only under `dist <= 5` in
// log_surv_increment(). The final branch is Weibull AFT (dist 5); a dist above
// 5 would be a programming error rather than a supported input, so it is
// rejected instead of silently returning a Weibull-AFT value.
real log_cumhaz_scalar(int dist, real t, real eta, real aux) {
  if (dist == 1) return log(t) + eta;
  if (dist == 2) return aux * log(t) + eta;
  if (dist == 3)
    return eta - log(aux) + aux * t + log1m_exp(-aux * t);
  if (dist == 4) return log(t) - eta;
  if (dist == 5) return aux * (log(t) - eta);
  reject("log_cumhaz_scalar() has no closed form for dist = ", dist);
  // Unreachable: reject() throws. Present because a non-void Stan function
  // must end in a return to satisfy the compiler's return guarantee, which
  // older stanc versions do not infer from a trailing reject().
  return not_a_number();
}

// log(exp(z) - 1) where the argument arrives as log(z), which is how the
// Gompertz and Weibull cumulative-hazard differences below reach it.
real log_expm1_from_log_x(real log_x) {
  if (log_x < -10) {
    real x = exp(log_x);
    return log_x + log1p(0.5 * x + square(x) / 6);
  }
  // The overflow is in expm1(z), which leaves double precision at z near 709,
  // so the guard belongs on z = exp(log_x) rather than on log_x itself. Tested
  // against log_x, it fired only above exp(700), and every log_x between
  // log(709) and 700 returned log(inf) = inf: for a Gompertz fit with delayed
  // entry or interval censoring that made the log-likelihood -inf and the draw
  // was rejected, at parameter values where the true log difference is an
  // ordinary finite number. Above the threshold log(e^z - 1) is z to well
  // under double precision.
  if (log_x > log(700)) return exp(log_x);
  return log(expm1(exp(log_x)));
}

// log(t_upper / t_lower) without cancellation for close bounds and without
// overflow for a lower bound near the bottom of the double range. log1p of
// the relative gap is exact for close bounds, and the quotient it takes
// overflows to +inf once t_lower is a subnormal, where the difference of the
// two logs has no cancellation left to fear.
real log_time_ratio(real t_upper, real t_lower) {
  if (t_lower < t_upper * 1e-8) return log(t_upper) - log(t_lower);
  return log1p((t_upper - t_lower) / t_lower);
}

// log(1 - exp(-a b)) for positive a and b whose product can underflow to
// zero: a Gompertz shape of 1e-310 times an interval of 1e-14, say. There the
// value is log(a) + log(b) to within a relative a b / 2, which is the same
// number the log-scale route took before the difference was rewritten.
real log1m_exp_neg_prod(real a, real b) {
  real ab = a * b;
  if (ab == 0) return log(a) + log(b);
  return log1m_exp(-ab);
}

// log(H(u) - H(l)). The Weibull and Gompertz differences are written from
// the UPPER bound, as a log H(u) + log(1 - exp(-(a log(u / l)))), rather
// than from the lower bound as a log t_l + log(expm1(a log(u / l))). The two
// are the same number, but with a shape of 3e14, l = 0.5 and u a hair under
// 1 the lower form adds -2e14 to +2e14 and keeps rounding of order 0.05 in
// a value near -2.3, while the upper form's first term is that small value
// itself and its second is zero. Where a log(u / l) is small the two forms
// agree to rounding, so nothing is lost on narrow intervals.
real log_cumhaz_diff(int dist, real t_upper, real t_lower, real eta,
                     real aux) {
  real dt = t_upper - t_lower;
  if (dt == 0) return negative_infinity();
  if (dist == 1) return eta + log(dt);
  if (dist == 4) return -eta + log(dt);
  if (dist == 3)
    return eta - log(aux) + aux * t_upper + log1m_exp_neg_prod(aux, dt);
  if (t_lower == 0) return log_cumhaz_scalar(dist, t_upper, eta, aux);
  {
    real log_tail = log1m_exp_neg_prod(aux, log_time_ratio(t_upper,
                                                           t_lower));
    if (dist == 2) return eta + aux * log(t_upper) + log_tail;
    // Weibull AFT: the shape multiplies log(t_upper) - eta as one small
    // difference. As aux * log(t_upper) - aux * eta the two products are
    // each 1e16 at a shape of 1e17 with the bound near exp(eta), and their
    // rounding swallowed a difference of -1.39.
    return aux * (log(t_upper) - eta) + log_tail;
  }
}

// log S(upper) - log S(lower), with a cumulative-hazard difference for the
// five exponential/Weibull/Gompertz families.
real log_surv_increment(int dist, real t_upper, real t_lower, real eta,
                        real aux, real aux2) {
  if (dist <= 5)
    return -exp(log_cumhaz_diff(dist, t_upper, t_lower, eta, aux));
  if (t_lower == 0)
    return log_surv_scalar(dist, t_upper, eta, aux, aux2);
  {
    real log_ratio = log_time_ratio(t_upper, t_lower);
    if (dist == 6) {
      real z_lower = (log(t_lower) - eta) / aux;
      real dz = log_ratio / aux;
      real z_upper = z_lower + dz;
      if (z_lower > 5) {
        return -0.5 * dz * (z_upper + z_lower)
               - (log_std_normal_hazard(z_upper)
                  - log_std_normal_hazard(z_lower));
      }
    } else if (dist == 7) {
      real z_lower = aux * (log(t_lower) - eta);
      if (z_lower > 0) {
        real dz = aux * log_ratio;
        real z_upper = z_lower + dz;
        return -dz - (log1p_exp(-z_upper) - log1p_exp(-z_lower));
      }
    } else {
      real shape = dist == 8 ? aux : aux2;
      real log_x_lower;
      real dlog_x;
      if (dist == 8) {
        log_x_lower = log(t_lower) - eta;
        dlog_x = log_ratio;
      } else {
        real Q = inv(sqrt(aux2));
        log_x_lower = log(aux2) + Q * (log(t_lower) - eta) / aux;
        dlog_x = Q * log_ratio / aux;
      }
      if (log_x_lower > log(shape + fmax(1, sqrt(shape)))) {
        // `log(expm1(dlog_x))` is formed BEFORE the guard below, so the
        // guard cannot protect it: a generalized gamma with sigma 0.0009
        // over (1, 2] has dlog_x = log(2) / 0.0009, about 770, where
        // expm1() leaves the double range. The value survives, since
        // log(inf) is inf and the guard then returns -inf, but the reverse
        // sweep does not: d log(y) / dy at y = inf is 0 and d expm1(z) / dz
        // at z = 770 is inf, and their product is NaN. CmdStan rejects the
        // initial value of `surv_ll_status(9, 2, 1, 0, 3, -0.009, 0.0009,
        // 1)` for a non-finite gradient at a finite log density. The helper
        // is the same function without the intermediate, returning z itself
        // above exp(700), where log(e^z - 1) is z to well under double
        // precision.
        real log_dx = log_x_lower + log_expm1_from_log_x(log(dlog_x));
        // The increment of the incomplete-gamma argument can overflow: a
        // generalized gamma with sigma 0.0009 over (1, 2] has an upper
        // argument of exp(780). The survival ratio is then zero to double
        // precision and the increment -inf, which the caller reads as an
        // interval holding everything that remains; the continued-fraction
        // factor at an infinite argument gave NaN instead, and the interval
        // fell to a quadrature whose grid cannot see the layer holding its
        // mass. The decision is on the increment, never on the endpoints:
        // a Gamma of shape 1 at eta = -710 has both cumulative hazards
        // beyond the double range at entry 1 and one ULP above it, and
        // their difference, exp(710) * 2^-52, is a finite log likelihood
        // near -4.96e292 that an endpoint test threw away as -inf.
        if (log_dx > log_double_max())
          return negative_infinity();
        real dx = exp(log_dx);
        // An overflowing upper argument with a finite increment means the
        // increment itself is near the top of the double range. The tail
        // form below drops the continued-fraction factors' ratio, which is
        // exp(-dlog_x) times 1 + O(shape / x_lower), and that O() term is
        // at most shape / x_lower in the log against a value near -1e308:
        // nothing a double can see, whatever x_lower is.
        real log_x_upper = log_x_lower + dlog_x;
        if (log_x_upper > log_double_max())
          return -dx + (shape - 1) * dlog_x;
        {
          real x_lower = exp(log_x_lower);
          real x_upper = exp(log_x_upper);
          return -dx + shape * dlog_x
                 + log_gamma_q_cf_factor(shape, x_upper)
                 - log_gamma_q_cf_factor(shape, x_lower);
        }
      }
    }
  }
  return log_surv_scalar(dist, t_upper, eta, aux, aux2)
         - log_surv_scalar(dist, t_lower, eta, aux, aux2);
}

// Whether log_surv_increment() takes one of its tail branches for these
// inputs, where the increment is formed without differencing two survival
// logs and carries only rounding relative to itself. Mirrors the branch
// conditions above; the closed-form families always do, and the log-logistic
// never reaches here because its interval has its own closed form.
int surv_increment_is_analytic(int dist, real t_lower, real eta, real aux,
                               real aux2) {
  if (dist <= 5) return 1;
  if (t_lower == 0) return 1;
  if (dist == 6) return (log(t_lower) - eta) / aux > 5;
  if (dist == 7) return aux * (log(t_lower) - eta) > 0;
  {
    real shape = dist == 8 ? aux : aux2;
    real log_x_lower = dist == 8
                         ? log(t_lower) - eta
                         : log(aux2) + inv(sqrt(aux2)) * (log(t_lower) - eta)
                                       / aux;
    return log_x_lower > log(shape + fmax(1, sqrt(shape)));
  }
}

// Composite Simpson on the offset range [a, b] from the base log time s_l,
// on the integrand f(e^s) e^s accumulated with log_sum_exp, doubling the
// panel count until two successive estimates agree to 1e-9 in the log,
// which puts the finer one within a few 1e-11 of the value, since the
// difference of two Simpson estimates overstates the finer one's error
// about sixteenfold. Every node is s_l plus an offset, and the offset travels separately
// into the density: a node formed as exp(s_l + offset) and logged again
// loses any offset below an ULP of s_l, which is every node of a narrow
// interval. Returns NaN when eleven doublings do not converge.
real log_simpson_offsets(int dist, real s_l, real a, real b, real eta,
                         real aux, real aux2) {
  real width = b - a;
  real f_a = log_density_offset(dist, s_l, a, eta, aux, aux2) + s_l + a;
  real f_b = log_density_offset(dist, s_l, b, eta, aux, aux2) + s_l + b;
  int n = 8;
  real prev = not_a_number();
  real cur = negative_infinity();
  if (width <= 0) return negative_infinity();
  for (level in 1:11) {
    real h = width / n;
    vector[n + 1] terms;
    terms[1] = f_a;
    terms[n + 1] = f_b;
    for (i in 1:(n - 1)) {
      real shift = a + i * h;
      real w = (i % 2 == 1) ? log(4.0) : log(2.0);
      terms[i + 1] = log_density_offset(dist, s_l, shift, eta, aux, aux2)
                     + s_l + shift + w;
    }
    cur = log_sum_exp(terms) + log(h) - log(3.0);
    if (!is_nan(prev) && abs(cur - prev) < 1e-9) return cur;
    prev = cur;
    n = 2 * n;
  }
  return not_a_number();
}

// Log P(t_lower < T <= t_upper) by quadrature of the density, for an interval
// that neither difference resolves. A single Simpson pass over the whole
// interval serves when it converges. When the mass sits in a layer next to
// one endpoint far narrower than any grid, it does not, and the interval is
// then cut geometrically toward the heavier endpoint: piece j covers the
// distances from that endpoint between width / 2^(j + 1) and width / 2^j,
// so a layer of any scale down to width / 2^60 falls inside a piece where
// the integrand varies by a bounded factor, and the innermost sliver, where
// the density is constant to rounding, is its value times its length. A
// piece that still does not converge is dropped only when its upper bound,
// the larger endpoint value times its length, is below the total so far by
// 25 in the log, under 1.4e-11 of it; otherwise the value is unknown and
// NaN is returned, which the likelihood treats as a rejection rather than
// a number. Returning the last unconverged estimate put a generalized-gamma
// interval 8 log units high, a factor of 2700 in the likelihood.
real log_interval_prob_quad(int dist, real t_upper, real t_lower, real eta,
                            real aux, real aux2) {
  real s_l = log(t_lower);
  real width = log_time_ratio(t_upper, t_lower);
  real whole;
  real f_l;
  real f_u;
  real total = negative_infinity();
  int from_upper;
  if (t_upper <= t_lower) return negative_infinity();
  whole = log_simpson_offsets(dist, s_l, 0, width, eta, aux, aux2);
  if (!is_nan(whole)) return whole;
  f_l = log_density_offset(dist, s_l, 0, eta, aux, aux2) + s_l;
  f_u = log_density_offset(dist, s_l, width, eta, aux, aux2) + s_l + width;
  from_upper = f_u > f_l;
  // Innermost first, so the pieces that hold the mass are summed before
  // the far ones are judged against the total.
  {
    real sliver = width / 2^60;
    real edge = from_upper ? width - sliver : 0;
    total = log_density_offset(dist, s_l, edge, eta, aux, aux2) + s_l + edge
            + log(sliver);
  }
  for (k in 0:59) {
    int j = 59 - k;
    real far = width / 2^j;
    real near = far / 2;
    real a = from_upper ? width - far : near;
    real b = from_upper ? width - near : far;
    real piece = log_simpson_offsets(dist, s_l, a, b, eta, aux, aux2);
    if (is_nan(piece)) {
      // Unconverged, but bounded above by the larger endpoint value times
      // the length; negligible beside what is already summed means it does
      // not matter, and anything else means the answer is not known.
      real bound = fmax(log_density_offset(dist, s_l, a, eta, aux, aux2) + s_l
                        + a,
                        log_density_offset(dist, s_l, b, eta, aux, aux2) + s_l
                        + b) + log(b - a);
      if (bound < total - 25) continue;
      return not_a_number();
    }
    total = log_sum_exp(total, piece);
  }
  return total;
}

// log P(l < T <= u | T > e) for the log-logistic, in a closed form with no
// subtraction of nearby numbers. With z = a (log t - eta), F = 1 / (1 +
// e^{-z}), S = 1 / (1 + e^{z}) and g = log1p_exp,
//   log(F(u) - F(l)) = z_l + log(expm1(d)) - g(z_l) - g(z_u)
// for d = a log(u / l) and z_u = z_l + d. Written that way the terms in
// z_u and d cancel each other, and with a shape of 1e17 they are each near
// 7e16 while the answer is log(1/2): rounding in either swamped it. Using
// log(expm1(d)) = d + log1m_exp(-d) and g(z_u) = z_u + log1p_exp(-z_u), the
// large parts cancel algebraically and what is left is
//   log1m_exp(-d) - log1p_exp(-z_u) - g(z_l),
// every term of which is bounded by the size of the answer. Conditioning
// adds g(z_e), and g(z_e) - g(z_l) is taken by the signs of the two, with
// g(z) = max(z, 0) + log1p(e^{-|z|}): both negative gives a difference of
// two small log1p terms, both positive gives (z_e - z_l), the log of a time
// ratio, plus two small terms, and a straddle gives -z_l plus small terms.
// Two enormous g values are never differenced: with eta = -1e16 the sum of
// an unconditional log probability and -log S(e) returned 0 for a value of
// -0.46, and with a shape of 1e17 and both z far negative it returned 32
// for a value of 0. Unconditional is e = 0, where g(z_e) is 0.
real log_loglogistic_interval(real t_upper, real t_lower, real t_entry,
                              real eta, real aux) {
  // The lower and upper centered log times each have two forms, and each
  // fails somewhere. Formed from the previous point plus a log time ratio,
  // (log e - eta) + log(l / e) say, a centered log time keeps an offset
  // below an ULP of the log, but when the points straddle the center at a
  // large shape the two terms cancel, each near 0.7 for bounds 0.5 and a
  // hair under 1, and the shape turns the rounding into units. Formed
  // directly as log t - eta it is exact there, one small difference, but
  // two points an ULP apart have the same rounded log and the offset is
  // gone. The form is chosen by which point sits nearer the center: when
  // the later point is nearer, its own difference is the smaller and the
  // more accurate, and otherwise the two terms of the sum share a sign and
  // nothing cancels. Two points that straddle the center take the direct
  // form whatever their distances: at equal distances the sum is two
  // opposite terms that cancel to rounding. A difference of two scores is
  // then taken from the ratio it was built from, never from the two rounded
  // scores.
  real centered_e = t_entry > 0 ? log(t_entry) - eta : negative_infinity();
  real centered_l = log(t_lower) - eta;
  int lower_from_entry = 0;
  real lower_ratio = 0;
  real log_ratio = log_time_ratio(t_upper, t_lower);
  real centered_direct = log(t_upper) - eta;
  real centered_u;
  real z_l;
  real z_u;
  real lp;
  if (t_entry > 0 && t_entry != t_lower && centered_l * centered_e >= 0
      && !(abs(centered_l) < abs(centered_e))) {
    lower_ratio = log_time_ratio(t_lower, t_entry);
    centered_l = centered_e + lower_ratio;
    lower_from_entry = 1;
  } else if (t_entry == t_lower) {
    centered_l = centered_e;
    lower_from_entry = 1;
  }
  centered_u = centered_direct * centered_l < 0
                 || abs(centered_direct) < abs(centered_l)
                 ? centered_direct
                 : centered_l + log_ratio;
  z_l = aux * centered_l;
  z_u = aux * centered_u;
  lp = log1m_exp_neg_prod(aux, log_ratio) - log1p_exp(-z_u);
  if (t_entry > 0) {
    real z_e = aux * centered_e;
    if (z_e >= 0) {
      real drop = lower_from_entry
                    ? -aux * lower_ratio
                    : aux * (centered_e - centered_l);
      return lp + drop + log1p(exp(-z_e)) - log1p(exp(-z_l));
    }
    if (z_l >= 0) return lp + log1p(exp(z_e)) - z_l - log1p(exp(-z_l));
    return lp + log1p(exp(z_e)) - log1p(exp(z_l));
  }
  return lp - log1p_exp(z_l);
}

// Whether a CDF difference resolves the interval. The result is
//   log F(u) + log(1 - exp(log F(l) - log F(u))),
// and the two log CDFs each carry rounding near eps * |log F(u)|. The first
// term carries that rounding whatever route is taken; it is the rounding of
// the answer itself. The second term is where a difference goes wrong: an
// error e in the exponent moves it by e (1 - m) / m, with m the interval's
// mass as a fraction of F(u). So the cancellation error is
//   eps * |log F(u)| * (1 - m) / m,
// and the difference is trusted when that is below 1e-11, which is m above
// 1e-5 * max(1, |log F(u)|), OR when (1 - m) / m is at most one, which is m
// of at least one half: the cancellation then adds no more than the answer's
// own rounding, however deep the tail. That second clause is what keeps a
// wide lower-tail interval on the difference. With a log-normal at sigma
// 1e-4 and eta = log(2) + 1, the interval (1, 2] has log F(2) near -5e7 and
// F(1) negligible beside it; the answer is log F(2) to eleven digits, while
// the density's mass sits in a layer next to the upper bound far narrower
// than any grid, and the quadrature came back 9.3 log units high.
int cdf_diff_resolves(real log_cdf_upper, real log_cdf_lower) {
  real mass;
  if (log_cdf_upper <= log_cdf_lower) return 0;
  mass = -expm1(log_cdf_lower - log_cdf_upper);
  return mass >= 0.5 || mass > 1e-5 * fmax(1, -log_cdf_upper);
}

// The same test for the survival increment log S(u) - log S(l). Formed by
// differencing two survival logs its error is near eps * |log S(l)| and the
// same two clauses apply. Formed in a tail branch it carries only rounding
// relative to itself, so any negative value resolves the interval, negative
// infinity included: that is an interval holding everything past S(l), for
// which log1m_exp() is exactly zero. A log-normal with sigma 5e-8 at t = 1
// has log S near -1.25e15 and an increment of -0.22 over one ULP, an
// interval holding a fifth of what remains; the quadrature cannot resolve
// the layer that mass sits in, and its result cancelled against
// log S(entry) to the wrong conditional value.
int surv_increment_resolves(real increment, real log_surv_lower,
                            int analytic) {
  real mass;
  if (is_nan(increment) || increment >= 0) return 0;
  // A -inf increment with a finite log S(l) is S(u) underflowing to zero
  // beside a representable S(l): the interval holds all of what remains to
  // within less than an ULP of it, whichever way the increment was formed.
  // Both survivals underflowing gives NaN above, not -inf.
  if (analytic || is_inf(increment)) return 1;
  mass = -expm1(increment);
  return mass >= 0.5 || mass > 1e-5 * fmax(1, -log_surv_lower);
}

// Log P(t_lower < T <= t_upper). The route is chosen by what resolves the
// interval, not by which one returns a finite number:
//
// * The five exponential/Weibull/Gompertz families have a closed-form
//   cumulative hazard whose difference is computed analytically, so
//   log S(l) + log(1 - exp(-dH)) is exact to rounding wherever the bounds
//   themselves are representable, and no other route is ever needed.
// * The log-logistic has the closed form above.
// * The log-normal, gamma and generalized gamma take the CDF difference in the
//   lower half and the survival increment in the upper half, each only when
//   the resolution test says its rounding is well below the mass, and fall
//   back to quadrature of the density otherwise.
real log_interval_prob_scalar(int dist, real t_upper, real t_lower, real eta,
                              real aux, real aux2) {
  if (t_upper <= t_lower) return negative_infinity();
  if (t_lower == 0) return log_cdf_scalar(dist, t_upper, eta, aux, aux2);
  if (dist <= 5)
    return log_surv_scalar(dist, t_lower, eta, aux, aux2)
           + log1m_exp_neg_exp(log_cumhaz_diff(dist, t_upper, t_lower, eta,
                                               aux));
  if (dist == 7)
    return log_loglogistic_interval(t_upper, t_lower, 0, eta, aux);
  {
    real log_cdf_upper = log_cdf_scalar(dist, t_upper, eta, aux, aux2);
    if (log_cdf_upper < -0.6931471805599453) {
      real log_cdf_lower = log_cdf_scalar(dist, t_lower, eta, aux, aux2);
      if (cdf_diff_resolves(log_cdf_upper, log_cdf_lower))
        return log_diff_exp(log_cdf_upper, log_cdf_lower);
    } else {
      real log_surv_lower = log_surv_scalar(dist, t_lower, eta, aux, aux2);
      real inc = log_surv_increment(dist, t_upper, t_lower, eta, aux, aux2);
      if (surv_increment_resolves(inc, log_surv_lower,
                                  surv_increment_is_analytic(dist, t_lower,
                                                             eta, aux, aux2)))
        return log_surv_lower + log1m_exp(inc);
    }
  }
  return log_interval_prob_quad(dist, t_upper, t_lower, eta, aux, aux2);
}

// Log P(t_lower < T <= t_upper | T > t_entry), for delayed entry. Left
// censoring passes t_lower == t_entry.
//
// The same routing as log_interval_prob_scalar(), conditioned on entry. For
// the closed-form families both factors, log S(l)/S(entry) = -dH(entry, l)
// and log(1 - S(u)/S(l)) = log(1 - exp(-dH(l, u))), come from analytic
// cumulative-hazard differences and are exact to rounding. For the others the
// CDF difference over S(entry) is the lower-tail form: F(u) < 1/2 forces
// S(entry) > 1/2, so log S(entry) is a small negative number and cannot
// reintroduce the huge right-tail terms the increment form exists to avoid.
// Gamma with shape 10 puts S at exactly 1.0 in double precision at t = 0.025,
// 0.05 and 0.1 alike, so every survival increment there is zero and only the
// CDF route has the value, which is -38.22216 for the interval (0.05, 0.1]
// with entry at 0.025. When neither difference resolves the interval, the
// quadrature does, and S(entry) is divided out afterward.
real log_cond_interval_prob(int dist, real t_upper, real t_lower, real t_entry,
                            real eta, real aux, real aux2) {
  if (t_upper <= t_lower) return negative_infinity();
  if (dist <= 5) {
    real log_reach = t_lower == t_entry
                       ? 0
                       : -exp(log_cumhaz_diff(dist, t_lower, t_entry, eta,
                                              aux));
    return log_reach
           + log1m_exp_neg_exp(log_cumhaz_diff(dist, t_upper, t_lower, eta,
                                               aux));
  }
  if (dist == 7)
    return log_loglogistic_interval(t_upper, t_lower, t_entry, eta, aux);
  {
    real log_cdf_upper = log_cdf_scalar(dist, t_upper, eta, aux, aux2);
    if (log_cdf_upper < -0.6931471805599453) {
      real log_cdf_lower = log_cdf_scalar(dist, t_lower, eta, aux, aux2);
      if (cdf_diff_resolves(log_cdf_upper, log_cdf_lower))
        return log_diff_exp(log_cdf_upper, log_cdf_lower)
               - log_surv_scalar(dist, t_entry, eta, aux, aux2);
    } else {
      real log_surv_lower = log_surv_scalar(dist, t_lower, eta, aux, aux2);
      real inc = log_surv_increment(dist, t_upper, t_lower, eta, aux, aux2);
      if (surv_increment_resolves(inc, log_surv_lower,
                                  surv_increment_is_analytic(dist, t_lower,
                                                             eta, aux, aux2)))
        return (t_lower == t_entry
                  ? 0
                  : log_surv_increment(dist, t_lower, t_entry, eta, aux, aux2))
               + log1m_exp(inc);
    }
  }
  return log_interval_prob_quad(dist, t_upper, t_lower, eta, aux, aux2)
         - log_surv_scalar(dist, t_entry, eta, aux, aux2);
}

// Status-aware single-observation log-likelihood with optional delayed entry.
real surv_ll_status(int dist, real time, real start_time, real delay_time,
                    int status, real eta, real aux, real aux2) {
  real l;
  if (status == 0) {            // right-censored: log S(t)
    if (delay_time > 0)
      l = log_surv_increment(dist, time, delay_time, eta, aux, aux2);
    else
      l = log_surv_scalar(dist, time, eta, aux, aux2);
  } else if (status == 1) {     // event: log h(t) + log S(t)  (or log f(t))
    if (delay_time > 0)
      l = log_haz_full(dist, time, eta, aux, aux2)
          + log_surv_increment(dist, time, delay_time, eta, aux, aux2);
    else
      l = log_density_scalar(dist, time, eta, aux, aux2);
  } else if (status == 2) {     // left-censored: event in (entry, t]
    // Under delayed entry this is the conditional probability of an event in
    // (delay, t]. With delay = 0 this reduces to log F(t).
    if (delay_time > 0)
      l = log_cond_interval_prob(dist, time, delay_time, delay_time, eta, aux,
                                 aux2);
    else
      l = log_cdf_scalar(dist, time, eta, aux, aux2);
  } else {                       // interval-censored: log(S(lower) - S(upper))
    // Under delayed entry the quantity is conditional on survival to entry,
    // P(lower < T <= upper | T > entry). Forming the unconditional interval
    // probability and then subtracting log S(entry) is that quantity
    // algebraically, but both terms grow without bound in the tail, so the
    // subtraction cancels away the significant digits and gives -Inf - -Inf,
    // i.e. NaN, once either underflows. Build it from increments instead:
    // log S(lower)/S(entry) + log[1 - S(upper)/S(lower)]. Each factor is a
    // conditional probability computed directly, and `log_surv_increment()`
    // already computes exactly these ratios stably. The three other status
    // branches above take that route for the same reason; this one did not.
    //
    // That form is right in the right tail and wrong in the left one, where
    // survival rounds to one and every increment collapses to zero.
    // `log_cond_interval_prob()` picks whichever of the two is representable.
    if (delay_time > 0)
      l = log_cond_interval_prob(dist, time, start_time, delay_time, eta, aux,
                                 aux2);
    else
      l = log_interval_prob_scalar(dist, time, start_time, eta, aux, aux2);
  }
  return l;
}


// Population-standardized (marginal) survival at time t: mean over a set of
// linear predictors of S(t | eta_i). This is S-bar(t) = E_x[S(t|x)].
real mean_surv(int dist, real t, vector eta, real aux, real aux2) {
  int n = num_elements(eta);
  vector[n] s;
  for (i in 1:n) s[i] = exp(log_surv_scalar(dist, t, eta[i], aux, aux2));
  return mean(s);
}

// Population-standardized (marginal) log survival at time t, in log space:
//   log S-bar(t) = log_sum_exp_i(log S(t|eta_i)) - log(n).
// Stays finite where the natural-scale mean(S) underflows to 0 deep in the tail,
// so the marginal cumulative hazard (cumhaz = -log S-bar) does not blow up to
// +inf at extreme prediction times (the same log-space form as log_mean_haz).
real log_mean_surv(int dist, real t, vector eta, real aux, real aux2) {
  int n = num_elements(eta);
  vector[n] log_s;
  for (i in 1:n) log_s[i] = log_surv_scalar(dist, t, eta[i], aux, aux2);
  return log_sum_exp(log_s) - log(n);
}

// Population-standardized (marginal) log hazard at time t:
//   log h-bar(t) = log E_x[f(t|x)] - log E_x[S(t|x)]   (= log of -d/dt log S-bar).
// Stays on the log scale (log_sum_exp of numerator and denominator), so the
// marginal log hazard ratio (predict type "loghr") is finite even where the
// natural-scale hazard underflows to 0 deep in the tail.
real log_mean_haz(int dist, real t, vector eta, real aux, real aux2) {
  int n = num_elements(eta);
  vector[n] log_s;
  vector[n] log_num;
  real max_log_s;
  for (i in 1:n) {
    log_s[i] = log_surv_scalar(dist, t, eta[i], aux, aux2);
  }
  max_log_s = max(log_s);
  if (is_inf(max_log_s)) {
    int best = 1;
    for (i in 2:n) {
      if ((dist <= 3 && eta[i] < eta[best])
          || (dist >= 4 && eta[i] > eta[best])) best = i;
    }
    return log_haz_full(dist, t, eta[best], aux, aux2);
  }
  for (i in 1:n) {
    log_s[i] -= max_log_s;
    log_num[i] = log_haz_full(dist, t, eta[i], aux, aux2) + log_s[i];
    if (is_nan(log_num[i]))
      log_num[i] = log_density_scalar(dist, t, eta[i], aux, aux2)
                   - max_log_s;
  }
  return log_sum_exp(log_num) - log_sum_exp(log_s);
}

// Restricted mean survival time over [grid[1], grid[g]] by the trapezoidal
// rule on the standardized survival curve. `grid` should start at 0.
real rmst_param(int dist, vector grid, vector eta, real aux, real aux2) {
  int g = num_elements(grid);
  vector[g] sbar;
  real area = 0;
  for (p in 1:g) sbar[p] = mean_surv(dist, grid[p], eta, aux, aux2);
  for (p in 2:g) area += 0.5 * (grid[p] - grid[p - 1]) * (sbar[p] + sbar[p - 1]);
  return area;
}
}

data {
  // IPD (index treatment)
  int<lower=1> n_ipd;
  int<lower=1> n_cov;
  matrix[n_ipd, n_cov] X_ipd;            // centered covariates (generated quantities)
  vector<lower=0>[n_ipd] ipd_time;
  vector<lower=0>[n_ipd] ipd_start_time;
  vector<lower=0>[n_ipd] ipd_delay_time;
  array[n_ipd] int<lower=0,upper=3> ipd_status;

  // Comparator pseudo-IPD (reconstructed from digitized KM curve)
  int<lower=1> n_agd;
  int<lower=1> n_agd_rows;
  vector<lower=0>[n_agd] agd_time;
  vector<lower=0>[n_agd] agd_start_time;
  vector<lower=0>[n_agd] agd_delay_time;
  array[n_agd] int<lower=0,upper=3> agd_status;
  array[n_agd] int<lower=1,upper=n_agd_rows> agd_arm;

  // Integration points (comparator covariate distribution, per arm)
  int<lower=1> n_int;
  array[n_agd_rows] matrix[n_int, n_cov] X_int;   // centered covariates (gen. quantities)

  // Combined (intercepts + index/comparator covariates) design and optional QR.
  int<lower=0,upper=1> qr;               // 1 = sample on the QR scale
  int<lower=1> nB;                       // design columns = 2 + 2 * n_cov
  matrix[n_ipd, nB] Xq_ipd;             // design for IPD rows (Q if qr = 1)
  array[n_agd_rows] matrix[n_int, nB] Xq_int;     // design for AgD integration rows
  matrix[nB, nB] R_inv;                  // R^{-1} (identity when qr = 0)

  // Distribution + link
  int<lower=1,upper=9> dist;
  int<lower=1,upper=1> link;

  // Baseline stratification (the analogue of multinma's aux_by).
  //   1 = one shape parameter shared by both studies (proportional hazards
  //       across studies, the historical behavior);
  //   2 = a separate shape per study, so the marginal hazard ratio is free to
  //       vary with time. The study intercepts stay identified because the
  //       distribution's scale is carried by exp(eta), not by the shape.
  int<lower=1,upper=2> n_strata;

  // HTA prediction grid
  int<lower=1> n_pred_times;
  vector<lower=0>[n_pred_times] pred_times;
  int<lower=2> n_rmst_grid;
  vector<lower=0>[n_rmst_grid] rmst_grid_times;

// Prior hyperparameters for ML-UMR intercepts and regression coefficients.
//
// This file is included from each model's data block *after* `n_cov` is
// declared. The four `_dist` / `_df` fields let the model branch on prior
// family at runtime without re-compiling.
//
//   prior_*_dist:  0 = normal, 1 = student_t
//   prior_*_df:    used only when dist == 1; otherwise any positive
//                  placeholder (Stan rejects non-positive df in `data`).
//
// `prior_beta_mean` and `prior_beta_sd` are vectors so that per-coefficient
// priors and `autoscale` (divide scale by sd(x_j)) are supported without
// another Stan model variant. For a scalar user-facing prior the R side
// broadcasts to length `n_cov`.

real prior_intercept_mean;
real<lower=0> prior_intercept_sd;
int<lower=0,upper=1> prior_intercept_dist;
real<lower=0> prior_intercept_df;

vector[n_cov] prior_beta_mean;
vector<lower=0>[n_cov] prior_beta_sd;
int<lower=0,upper=1> prior_beta_dist;
real<lower=0> prior_beta_df;

  // Fully separate prior for beta_comparator: its own family, degrees of
  // freedom, location and scale, none of them tied to beta_index. Tighten these
  // to regularize the comparator coefficients, which are identified only
  // through the AgD likelihood.
  vector[n_cov] prior_beta_comparator_mean;
  vector<lower=0>[n_cov] prior_beta_comparator_sd;
  int<lower=0,upper=1> prior_beta_comparator_dist;
  real<lower=0> prior_beta_comparator_df;

// Prior hyperparameters for the auxiliary shape/scale parameters of the
// parametric survival models. Included in the data block of
// mlumr_survival_{spfa,relaxed}.stan after `n_cov` is declared.
//
//   prior_aux_dist:  0 = half-normal, 1 = half-t, 2 = exponential
//                    (positivity comes from the <lower=0> parameter constraint;
//                     see log_prior_sigma() in priors_functions.stan)
//   prior_aux_df:    used only when dist == 1; otherwise a positive placeholder.
//
// `aux`  is the shape (Weibull/gamma), rate-of-increase (Gompertz), shape
//        (log-logistic), sdlog (log-normal), or first shape (gen. gamma).
// `aux2` is the second shape of the generalized gamma only.

real prior_aux_location;
real<lower=0> prior_aux_scale;
int<lower=0,upper=2> prior_aux_dist;
real<lower=0> prior_aux_df;

real prior_aux2_location;
real<lower=0> prior_aux2_scale;
int<lower=0,upper=2> prior_aux2_dist;
real<lower=0> prior_aux2_df;
}

transformed data {
  int nonexp = (dist != 1 && dist != 4) ? 1 : 0;
  int is_gengamma = (dist == 9) ? 1 : 0;
  int n_int_all = n_agd_rows * n_int;
  matrix[n_int_all, n_cov] X_int_all;
  for (a in 1:n_agd_rows)
    X_int_all[((a - 1) * n_int + 1):(a * n_int)] = X_int[a];
}

parameters {
  // Combined coefficients on the (QR) sampling scale:
  // [mu_index, mu_comparator, beta_index, beta_comparator].
  vector[nB] beta_tilde;
  matrix<lower=0>[nonexp, n_strata] aux_raw;
  matrix<lower=0>[is_gengamma, n_strata] aux2_raw;
}

transformed parameters {
  // Recover original-scale coefficients (affine map, no Jacobian needed).
  vector[nB] allbeta = qr ? R_inv * beta_tilde : beta_tilde;
  real mu_index = allbeta[1];
  real mu_comparator = allbeta[2];
  vector[n_cov] beta_index = segment(allbeta, 3, n_cov);
  vector[n_cov] beta_comparator = segment(allbeta, 3 + n_cov, n_cov);
  vector[n_ipd] eta_ipd = Xq_ipd * beta_tilde;
  // Stratum 1 is the index study; `n_strata` indexes the comparator, so with
  // n_strata = 1 both views are the same parameter and the model reduces to the
  // unstratified one exactly.
  real aux_val      = nonexp ? aux_raw[1, 1] : 1.0;
  real aux_val_cmp  = nonexp ? aux_raw[1, n_strata] : 1.0;
  real aux2_val     = is_gengamma ? aux2_raw[1, 1] : 1.0;
  real aux2_val_cmp = is_gengamma ? aux2_raw[1, n_strata] : 1.0;
}

model {
  // Priors
  target += log_prior_scalar(mu_index, prior_intercept_mean, prior_intercept_sd,
                             prior_intercept_dist, prior_intercept_df);
  target += log_prior_scalar(mu_comparator, prior_intercept_mean, prior_intercept_sd,
                             prior_intercept_dist, prior_intercept_df);
  target += log_prior_vector(beta_index, prior_beta_mean, prior_beta_sd,
                             prior_beta_dist, prior_beta_df);
  target += log_prior_vector(beta_comparator, prior_beta_comparator_mean,
                             prior_beta_comparator_sd, prior_beta_comparator_dist,
                             prior_beta_comparator_df);
  for (s in 1:n_strata) {
    if (nonexp)
      target += log_prior_sigma(aux_raw[1, s], prior_aux_location, prior_aux_scale,
                                prior_aux_dist, prior_aux_df);
    if (is_gengamma)
      target += log_prior_sigma(aux2_raw[1, s], prior_aux2_location, prior_aux2_scale,
                                prior_aux2_dist, prior_aux2_df);
  }

  // IPD likelihood (index treatment), status-aware.
  for (i in 1:n_ipd)
    target += surv_ll_status(dist, ipd_time[i], ipd_start_time[i],
                             ipd_delay_time[i], ipd_status[i],
                             eta_ipd[i], aux_val, aux2_val);

  // Comparator pseudo-IPD likelihood, integrated over comparator covariates.
  {
    // eta is the comparator design block times beta_tilde, recomputed for every
    // pseudo-individual.
    vector[n_agd] agd_ll;
    for (j in 1:n_agd) {
      int a = agd_arm[j];
      vector[n_int] eta = Xq_int[a] * beta_tilde;
      vector[n_int] ll;
      for (m in 1:n_int)
        ll[m] = surv_ll_status(dist, agd_time[j], agd_start_time[j],
                               agd_delay_time[j], agd_status[j],
                               eta[m], aux_val_cmp, aux2_val_cmp);
      agd_ll[j] = log_sum_exp(ll) - log(n_int);
    }
    target += sum(agd_ll);
  }
}

generated quantities {
  real delta_conditional = mu_index - mu_comparator;
  real delta_index;
  real delta_comparator;

  // Effect modification (difference in prognostic coefficients).
  vector[n_cov] delta_beta = beta_index - beta_comparator;

  array[n_pred_times] real surv_index_index;
  array[n_pred_times] real surv_comparator_index;
  array[n_pred_times] real surv_index_comparator;
  array[n_pred_times] real surv_comparator_comparator;
  array[n_pred_times] real haz_index_index;
  array[n_pred_times] real haz_comparator_index;
  array[n_pred_times] real haz_index_comparator;
  array[n_pred_times] real haz_comparator_comparator;
  array[n_pred_times] real cumhaz_index_index;
  array[n_pred_times] real cumhaz_comparator_index;
  array[n_pred_times] real cumhaz_index_comparator;
  array[n_pred_times] real cumhaz_comparator_comparator;
  // Marginal log hazard ratio (index vs comparator treatment) by population,
  // formed in log space so it stays finite where the natural-scale hazard
  // underflows (predict type "loghr").
  array[n_pred_times] real loghr_index;
  array[n_pred_times] real loghr_comparator;

  real rmst_index_index;
  real rmst_comparator_index;
  real rmst_index_comparator;
  real rmst_comparator_comparator;
  real rmst_diff_index;
  real rmst_diff_comparator;

  vector[n_ipd] log_lik_ipd;
  vector[n_agd] log_lik_agd;

  {
    vector[n_ipd] eta_idx_ip = mu_index + X_ipd * beta_index;           // index trt, index pop
    vector[n_ipd] eta_cmp_ip = mu_comparator + X_ipd * beta_comparator; // comp trt, index pop
    vector[n_int_all] eta_idx_cp = mu_index + X_int_all * beta_index;        // index trt, comp pop
    vector[n_int_all] eta_cmp_cp = mu_comparator + X_int_all * beta_comparator; // comp trt, comp pop

    if (dist <= 3) {  // PH: marginal log HR
      delta_index = log_sum_exp(eta_idx_ip) - log_sum_exp(eta_cmp_ip);
      delta_comparator = log_sum_exp(eta_idx_cp) - log_sum_exp(eta_cmp_cp);
    } else {          // AFT: conditional log time-ratio at the mean covariate value
      // eta is linear in x, so mean(eta) is the linear predictor at xbar; under
      // effect modification this average-covariate conditional log-TR is not the
      // collapsible marginal effect (use the RMST-based estimands for that).
      delta_index = mean(eta_idx_ip) - mean(eta_cmp_ip);
      delta_comparator = mean(eta_idx_cp) - mean(eta_cmp_cp);
    }

    for (p in 1:n_pred_times) {
      real t = pred_times[p];
      surv_index_index[p]           = mean_surv(dist, t, eta_idx_ip, aux_val, aux2_val);
      surv_comparator_index[p]      = mean_surv(dist, t, eta_cmp_ip, aux_val_cmp, aux2_val_cmp);
      surv_index_comparator[p]      = mean_surv(dist, t, eta_idx_cp, aux_val, aux2_val);
      surv_comparator_comparator[p] = mean_surv(dist, t, eta_cmp_cp, aux_val_cmp, aux2_val_cmp);
      real lmh_ii = log_mean_haz(dist, t, eta_idx_ip, aux_val, aux2_val);
      real lmh_ci = log_mean_haz(dist, t, eta_cmp_ip, aux_val_cmp, aux2_val_cmp);
      real lmh_ic = log_mean_haz(dist, t, eta_idx_cp, aux_val, aux2_val);
      real lmh_cc = log_mean_haz(dist, t, eta_cmp_cp, aux_val_cmp, aux2_val_cmp);
      haz_index_index[p]            = exp(lmh_ii);
      haz_comparator_index[p]       = exp(lmh_ci);
      haz_index_comparator[p]       = exp(lmh_ic);
      haz_comparator_comparator[p]  = exp(lmh_cc);
      loghr_index[p]                = lmh_ii - lmh_ci;
      loghr_comparator[p]           = lmh_ic - lmh_cc;
      cumhaz_index_index[p]            = -log_mean_surv(dist, t, eta_idx_ip, aux_val, aux2_val);
      cumhaz_comparator_index[p]       = -log_mean_surv(dist, t, eta_cmp_ip, aux_val_cmp, aux2_val_cmp);
      cumhaz_index_comparator[p]       = -log_mean_surv(dist, t, eta_idx_cp, aux_val, aux2_val);
      cumhaz_comparator_comparator[p]  = -log_mean_surv(dist, t, eta_cmp_cp, aux_val_cmp, aux2_val_cmp);
    }

    // delta_* is the MARGINAL log hazard ratio at the START of follow-up
    // (the t -> 0 limit), and it is NOT constant in time. With one shared
    // baseline h0(t) cancels from the CONDITIONAL hazard ratio, which is then
    // exactly exp(eta_i - eta_c) for every t; the MARGINAL ratio still drifts,
    // because it weights the covariate distribution by each arm's own survival
    // (E_x[h S] / E_x[S]) and the two risk sets diverge as follow-up proceeds.
    // The closed form below is therefore the marginal ratio at t -> 0, where
    // the two weightings still coincide. With a stratified
    // baseline it does not cancel: the true contrast picks up an extra
    // log(h0_index(t) / h0_comparator(t)) term and is no longer constant. Rather
    // than report a quantity that silently means something different, take the
    // value straight from the time-varying marginal hazards at the first
    // prediction time, which is what loghr_* already computes with the correct
    // per-study baselines.
    // Only for PROPORTIONAL-HAZARDS distributions that actually have a shape to
    // stratify. For AFT (dist > 3) delta_* is a location contrast and stays a
    // time-ratio functional, so replacing it with a hazard ratio would relabel
    // the estimand; for the exponential (nonexp = 0) there is no shape, so
    // stratification does not change the baseline and the closed form is exact.
    if (n_strata > 1 && nonexp && dist <= 3) {
      delta_index      = loghr_index[1];
      delta_comparator = loghr_comparator[1];
    }

    rmst_index_index           = rmst_param(dist, rmst_grid_times, eta_idx_ip, aux_val, aux2_val);
    rmst_comparator_index      = rmst_param(dist, rmst_grid_times, eta_cmp_ip, aux_val_cmp, aux2_val_cmp);
    rmst_index_comparator      = rmst_param(dist, rmst_grid_times, eta_idx_cp, aux_val, aux2_val);
    rmst_comparator_comparator = rmst_param(dist, rmst_grid_times, eta_cmp_cp, aux_val_cmp, aux2_val_cmp);
    rmst_diff_index      = rmst_index_index - rmst_comparator_index;
    rmst_diff_comparator = rmst_index_comparator - rmst_comparator_comparator;

    for (i in 1:n_ipd)
      log_lik_ipd[i] = surv_ll_status(dist, ipd_time[i], ipd_start_time[i],
                                      ipd_delay_time[i], ipd_status[i],
                                      eta_ipd[i], aux_val, aux2_val);
    {
      for (j in 1:n_agd) {
        int a = agd_arm[j];
        vector[n_int] eta = Xq_int[a] * beta_tilde;
        vector[n_int] ll;
        for (m in 1:n_int)
          ll[m] = surv_ll_status(dist, agd_time[j], agd_start_time[j],
                                 agd_delay_time[j], agd_status[j],
                                 eta[m], aux_val_cmp, aux2_val_cmp);
        log_lik_agd[j] = log_sum_exp(ll) - log(n_int);
      }
    }
  }
}
