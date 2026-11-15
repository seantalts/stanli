/*
    EpiNow2 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    EpiNow2 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with EpiNow2.  If not, see <http://www.gnu.org/licenses/>.
*/
functions {
// Stan functions from primarycensored version 1.5.1
real expgrowth_lpdf(real x, real xmin, real xmax, real r) {
  if (x < xmin || x > xmax) {
    return negative_infinity();
  }
  if (abs(r) < 1e-10) {
    return -log(xmax - xmin);
  }
  return log(abs(r)) + r * x -
    log(abs(exp(r * xmax) - exp(r * xmin)));
}
int check_for_analytical(int dist_id, int primary_id) {
  if (dist_id == 2 && primary_id == 1) return 1; // Gamma delay with Uniform primary
  if (dist_id == 1 && primary_id == 1) return 1; // Lognormal delay with Uniform primary
  if (dist_id == 3 && primary_id == 1) return 1; // Weibull delay with Uniform primary
  return 0; // No analytical solution for other combinations
}
real primarycensored_gamma_uniform_lcdf(data real d, real q, array[] real params, data real pwindow) {
  real shape = params[1];
  real rate = params[2];
  real log_window = log(pwindow);
  // log E where E = k * theta = shape / rate is the mean of the delay
  real log_E = log(shape) - log(rate);

  // F_T(d; k) and the recursion to F_T(d; k+1):
  // P(k+1, y) = P(k, y) - y^k e^{-y} / Gamma(k+1), with y = rate * d
  real log_F_T_d_k = gamma_lcdf(d | shape, rate);
  real gamma_kp1_pdf_log_d
    = shape * log(rate * d) - rate * d - lgamma(shape + 1);
  real log_F_T_d_kp1 = log_diff_exp(log_F_T_d_k, gamma_kp1_pdf_log_d);

  // q-dependent terms. Final algebra is unified; only a guard to avoid
  // log_diff_exp(-inf, -inf) and log(0) when q == 0 (q is data, so autodiff
  // is unaffected by this branch).
  real log_q_F_T_q;    // log(q * F_T(q; k))
  real log_E_tF_T_q;   // log(E * F_T(q; k+1))
  if (q > 0) {
    real log_F_T_q_k = gamma_lcdf(q | shape, rate);
    real gamma_kp1_pdf_log_q
      = shape * log(rate * q) - rate * q - lgamma(shape + 1);
    real log_F_T_q_kp1 = log_diff_exp(log_F_T_q_k, gamma_kp1_pdf_log_q);
    log_q_F_T_q = log(q) + log_F_T_q_k;
    log_E_tF_T_q = log_E + log_F_T_q_kp1;
  } else {
    log_q_F_T_q = negative_infinity();
    log_E_tF_T_q = negative_infinity();
  }

  // Unified form: F_{S+}(d) = (A - B) / w_P with A, B sums of positives:
  //   A = d * F_T(d; k)   + E * F_T(q; k+1)
  //   B = q * F_T(q; k)   + E * F_T(d; k+1)
  // Ordering A >= B is guaranteed by F_{S+}(d) >= 0.
  real log_A = log_sum_exp(log(d) + log_F_T_d_k, log_E_tF_T_q);
  real log_B = log_sum_exp(log_q_F_T_q, log_E + log_F_T_d_kp1);

  return log_diff_exp(log_A, log_B) - log_window;
}
real primarycensored_lognormal_uniform_lcdf(data real d, real q, array[] real params, data real pwindow) {
  real mu = params[1];
  real sigma = params[2];
  real mu_sigma2 = mu + square(sigma);
  real log_window = log(pwindow);
  // log E where E = exp(mu + sigma^2/2) is the mean of the delay
  real log_E = mu + 0.5 * square(sigma);

  real log_F_T_d = lognormal_lcdf(d | mu, sigma);
  real log_tF_T_d = lognormal_lcdf(d | mu_sigma2, sigma);

  // q-dependent terms (guard only to avoid log(0); final algebra is unified).
  real log_q_F_T_q;    // log(q * F_T(q))
  real log_E_tF_T_q;   // log(E * tilde F_T(q))
  if (q > 0) {
    real log_F_T_q = lognormal_lcdf(q | mu, sigma);
    real log_tF_T_q = lognormal_lcdf(q | mu_sigma2, sigma);
    log_q_F_T_q = log(q) + log_F_T_q;
    log_E_tF_T_q = log_E + log_tF_T_q;
  } else {
    log_q_F_T_q = negative_infinity();
    log_E_tF_T_q = negative_infinity();
  }

  // Unified form: F_{S+}(d) = (A - B) / w_P with
  //   A = d * F_T(d) + E * tilde F_T(q)
  //   B = q * F_T(q) + E * tilde F_T(d)
  // Ordering A >= B is guaranteed by F_{S+}(d) >= 0.
  real log_A = log_sum_exp(log(d) + log_F_T_d, log_E_tF_T_q);
  real log_B = log_sum_exp(log_q_F_T_q, log_E + log_tF_T_d);

  return log_diff_exp(log_A, log_B) - log_window;
}
real log_weibull_g(real t, real shape, real scale) {
  real x = pow(t * inv(scale), shape);
  real a = 1 + inv(shape);
  return log(gamma_p(a, x)) + lgamma(a);
}
real primarycensored_weibull_uniform_lcdf(data real d, real q, array[] real params, data real pwindow) {
  real shape = params[1];
  real scale = params[2];
  real log_window = log(pwindow);
  real log_scale = log(scale);

  // For Weibull: E = scale (lambda) and tilde F_T(t) = g(t; lambda, k), so
  // log(E * tilde F_T(t)) = log(scale) + log_weibull_g(t, shape, scale).
  real log_F_T_d = weibull_lcdf(d | shape, scale);
  real log_E_tF_T_d = log_scale + log_weibull_g(d, shape, scale);

  // q-dependent terms (guard only to avoid log(0); final algebra is unified).
  real log_q_F_T_q;    // log(q * F_T(q))
  real log_E_tF_T_q;   // log(E * tilde F_T(q)) = log(scale * g(q; lambda, k))
  if (q > 0) {
    log_q_F_T_q = log(q) + weibull_lcdf(q | shape, scale);
    log_E_tF_T_q = log_scale + log_weibull_g(q, shape, scale);
  } else {
    log_q_F_T_q = negative_infinity();
    log_E_tF_T_q = negative_infinity();
  }

  // Unified form: F_{S+}(d) = (A - B) / w_P with
  //   A = d * F_T(d)    + scale * g(q; lambda, k)
  //   B = q * F_T(q)    + scale * g(d; lambda, k)
  // Ordering A >= B is guaranteed by F_{S+}(d) >= 0.
  real log_A = log_sum_exp(log(d) + log_F_T_d, log_E_tF_T_q);
  real log_B = log_sum_exp(log_q_F_T_q, log_E_tF_T_d);

  return log_diff_exp(log_A, log_B) - log_window;
}
real primarycensored_analytical_lcdf_raw(data real d, int dist_id,
                                         array[] real params,
                                         data real pwindow,
                                         int primary_id) {
  real q = max({d - pwindow, 0});

  if (dist_id == 2 && primary_id == 1) {
    return primarycensored_gamma_uniform_lcdf(d | q, params, pwindow);
  } else if (dist_id == 1 && primary_id == 1) {
    return primarycensored_lognormal_uniform_lcdf(d | q, params, pwindow);
  } else if (dist_id == 3 && primary_id == 1) {
    return primarycensored_weibull_uniform_lcdf(d | q, params, pwindow);
  }
  return negative_infinity();
}
real primarycensored_analytical_lcdf(data real d, int dist_id,
                                           array[] real params,
                                           data real pwindow, data real L,
                                           data real D, int primary_id,
                                           array[] real primary_params) {
  if (d <= L) return negative_infinity();
  if (d >= D) return 0;

  real result = primarycensored_analytical_lcdf_raw(
    d, dist_id, params, pwindow, primary_id
  );

  // Apply truncation normalization
  if (!is_inf(D) || L > 0) {
    vector[2] bounds = primarycensored_truncation_bounds(
      L, D, dist_id, params, pwindow, primary_id, primary_params
    );
    real log_cdf_L = bounds[1];
    real log_cdf_D = bounds[2];

    real log_normalizer = primarycensored_log_normalizer(log_cdf_D, log_cdf_L, L);
    result = primarycensored_apply_truncation(result, log_cdf_L, log_normalizer, L);
  }

  return result;
}
real primarycensored_analytical_cdf(data real d, int dist_id,
                                          array[] real params,
                                          data real pwindow, data real L,
                                          data real D, int primary_id,
                                          array[] real primary_params) {
  return exp(primarycensored_analytical_lcdf(d | dist_id, params, pwindow, L, D, primary_id, primary_params));
}
int dist_has_positive_support(data int dist_id) {
  if (dist_id == 1) return 1;   // Lognormal
  if (dist_id == 2) return 1;   // Gamma
  if (dist_id == 3) return 1;   // Weibull
  if (dist_id == 4) return 1;   // Exponential
  if (dist_id == 9) return 1;   // Beta (support on [0, 1])
  if (dist_id == 13) return 1;  // Chi-square
  if (dist_id == 16) return 1;  // Inverse Gamma
  if (dist_id == 19) return 1;  // Inverse Chi-square
  if (dist_id == 21) return 1;  // Pareto
  if (dist_id == 22) return 1;  // Scaled inverse Chi-square
  return 0;
}
real dist_lcdf(real delay, array[] real params, int dist_id) {
  if (dist_has_positive_support(dist_id) && delay <= 0) {
    return negative_infinity();
  }

  // IDs match pcd_distributions$stan_id in R
  if (dist_id == 1) return lognormal_lcdf(delay | params[1], params[2]);
  else if (dist_id == 2) return gamma_lcdf(delay | params[1], params[2]);
  else if (dist_id == 3) return weibull_lcdf(delay | params[1], params[2]);
  else if (dist_id == 4) return exponential_lcdf(delay | params[1]);
  else if (dist_id == 9) return beta_lcdf(delay | params[1], params[2]);
  else if (dist_id == 12) return cauchy_lcdf(delay | params[1], params[2]);
  else if (dist_id == 13) return chi_square_lcdf(delay | params[1]);
  else if (dist_id == 15) return gumbel_lcdf(delay | params[1], params[2]);
  else if (dist_id == 16) return inv_gamma_lcdf(delay | params[1], params[2]);
  else if (dist_id == 17) return logistic_lcdf(delay | params[1], params[2]);
  else if (dist_id == 18) return normal_lcdf(delay | params[1], params[2]);
  else if (dist_id == 19) return inv_chi_square_lcdf(delay | params[1]);
  else if (dist_id == 20) return double_exponential_lcdf(delay | params[1], params[2]);
  else if (dist_id == 21) return pareto_lcdf(delay | params[1], params[2]);
  else if (dist_id == 22) return scaled_inv_chi_square_lcdf(delay | params[1], params[2]);
  else if (dist_id == 23) return student_t_lcdf(delay | params[1], params[2], params[3]);
  else if (dist_id == 24) return uniform_lcdf(delay | params[1], params[2]);
  else if (dist_id == 25) return von_mises_lcdf(delay | params[1], params[2]);
  else reject("Invalid distribution identifier: ", dist_id);
}
real primary_lpdf(real x, int primary_id, array[] real params, real xmin, real xmax) {
  // Implement switch for different primary distributions
  if (primary_id == 1) return uniform_lpdf(x | xmin, xmax);
  if (primary_id == 2) return expgrowth_lpdf(x | xmin, xmax, params[1]);
  // Add more primary distributions as needed
  reject("Invalid primary distribution identifier");
}
vector primarycensored_ode(real t, vector y, array[] real theta,
                            array[] real x_r, array[] int x_i) {
  real d = x_r[1];
  int dist_id = x_i[1];
  int primary_id = x_i[2];
  real pwindow = x_r[2];
  int dist_params_len = x_i[3];
  int primary_params_len = x_i[4];

  // Extract distribution parameters
  array[dist_params_len] real params;
  if (dist_params_len) {
    params = theta[1:dist_params_len];
  }
  array[primary_params_len] real primary_params;
  if (primary_params_len) {
    int primary_loc = num_elements(theta);
    primary_params = theta[primary_loc - primary_params_len + 1:primary_loc];
  }

  real log_cdf = dist_lcdf(t | params, dist_id);
  real log_primary_pdf = primary_lpdf(d - t | primary_id, primary_params, 0, pwindow);

  return rep_vector(exp(log_cdf + log_primary_pdf), 1);
}
real primarycensored_log_normalizer(real log_cdf_D, real log_cdf_L, real L) {
  if (!is_inf(L)) {
    return log_diff_exp(log_cdf_D, log_cdf_L);
  } else {
    return log_cdf_D;
  }
}
real primarycensored_apply_truncation(real log_cdf, real log_cdf_L,
                                      real log_normalizer, real L) {
  if (!is_inf(L)) {
    return log_diff_exp(log_cdf, log_cdf_L) - log_normalizer;
  } else {
    return log_cdf - log_normalizer;
  }
}
vector primarycensored_truncation_bounds(
  data real L, data real D,
  data int dist_id, array[] real params, data real pwindow,
  data int primary_id, array[] real primary_params
) {
  vector[2] result;
  // Internal lower bound for the un-truncated distribution: 0 lets the
  // `d <= L` early-exit in primarycensored_lcdf return -inf for delays below
  // the natural support of positive-support distributions; -inf disables that
  // short-circuit so distributions with support on the reals are integrated.
  // Expression is inlined (rather than bound to a local) so Stan's data-flow
  // checker recognises it as data-only.

  // Get CDF at lower truncation point L
  if (is_inf(L)) {
    result[1] = negative_infinity();
  } else {
    result[1] = primarycensored_lcdf(
      L | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    );
  }

  // Get CDF at upper truncation point D
  if (is_inf(D)) {
    result[2] = 0;
  } else {
    result[2] = primarycensored_lcdf(
      D | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    );
  }

  return result;
}
real primarycensored_cdf(data real d, data int dist_id, array[] real params,
                               data real pwindow, data real L, data real D,
                               data int primary_id,
                               array[] real primary_params) {
  real result;
  if (d <= L) {
    return 0;
  }

  if (d >= D) {
    return 1;
  }

  // Check if an analytical solution exists
  if (check_for_analytical(dist_id, primary_id)) {
    // Use analytical solution
    result = primarycensored_analytical_cdf(
      d | dist_id, params, pwindow, L, D, primary_id, primary_params
    );
  } else {
    // Use numerical integration for other cases. The integration variable
    // ranges over the primary-event time, so the natural lower bound is
    // d - pwindow. For positive-support delays the integrand `F_delay(t)` is
    // 0 for t <= 0, so an unclipped lower bound just adds a flat zero region
    // for negative t. Distributions with support on the reals also accept the
    // unclipped lower bound directly.
    real lower_bound = d - pwindow;
    int n_params = num_elements(params);
    int n_primary_params = num_elements(primary_params);
    array[n_params + n_primary_params] real theta = append_array(params, primary_params);
    array[4] int ids = {dist_id, primary_id, n_params, n_primary_params};

    vector[1] y0 = rep_vector(0.0, 1);
    result = ode_rk45(primarycensored_ode, y0, lower_bound, {d}, theta, {d, pwindow}, ids)[1, 1];

    // Apply truncation normalization on log scale for numerical stability.
    // Skip when F(L) = 0 makes it a no-op (positive support, L <= 0).
    if (!is_inf(D) || L > 0 ||
        (!is_inf(L) && !dist_has_positive_support(dist_id))) {
      real log_result = log(result);
      vector[2] bounds = primarycensored_truncation_bounds(
        L, D, dist_id, params, pwindow, primary_id, primary_params
      );
      real log_cdf_L = bounds[1];
      real log_cdf_D = bounds[2];

      real log_normalizer = primarycensored_log_normalizer(log_cdf_D, log_cdf_L, L);
      log_result = primarycensored_apply_truncation(
        log_result, log_cdf_L, log_normalizer, L
      );
      result = exp(log_result);
    }
  }

  return result;
}
real primarycensored_lcdf(data real d, data int dist_id, array[] real params,
                                data real pwindow, data real L, data real D,
                                data int primary_id,
                                array[] real primary_params) {
  real result;

  if (d <= L) {
    return negative_infinity();
  }

  if (d >= D) {
    return 0;
  }

  // Check if an analytical solution exists. The internal lower bound is 0 for
  // positive-support delays (lets the d <= L early-exit return -inf for d <= 0)
  // and -inf for distributions with support on the reals.
  if (check_for_analytical(dist_id, primary_id)) {
    result = primarycensored_analytical_lcdf(
      d | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    );
  } else {
    // Use numerical integration
    result = log(primarycensored_cdf(
      d | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    ));
  }

  // Handle truncation normalization. Skip when F(L) = 0 makes it a no-op
  // (positive support, L <= 0) to avoid the cancelling log_diff_exp.
  if (!is_inf(D) || L > 0 ||
      (!is_inf(L) && !dist_has_positive_support(dist_id))) {
    vector[2] bounds = primarycensored_truncation_bounds(
      L, D, dist_id, params, pwindow, primary_id, primary_params
    );
    real log_cdf_L = bounds[1];
    real log_cdf_D = bounds[2];

    real log_normalizer = primarycensored_log_normalizer(log_cdf_D, log_cdf_L, L);
    result = primarycensored_apply_truncation(result, log_cdf_L, log_normalizer, L);
  }

  return result;
}
real primarycensored_lpmf(data int d, data int dist_id, array[] real params,
                                data real pwindow, data real d_upper,
                                data real L, data real D, data int primary_id,
                                array[] real primary_params) {
  if (d_upper > D) {
    reject("Upper truncation point is greater than D. It is ", d_upper,
           " and D is ", D, ". Resolve this by increasing D to be greater or equal to d + swindow or decreasing swindow.");
  }
  if (d_upper <= d) {
    reject("Upper truncation point is less than or equal to d. It is ", d_upper,
           " and d is ", d, ". Resolve this by increasing d to be less than d_upper.");
  }
  if (d < L) {
    return negative_infinity();
  }
  real log_cdf_upper = primarycensored_lcdf(
    d_upper | dist_id, params, pwindow,
    dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
    positive_infinity(), primary_id, primary_params
  );
  real log_cdf_lower = primarycensored_lcdf(
    d | dist_id, params, pwindow,
    dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
    positive_infinity(), primary_id, primary_params
  );

  // Apply truncation normalization: log((F(d_upper) - F(d)) / (F(D) - F(L))).
  // Skip when F(L) = 0 makes it a no-op (positive support, L <= 0).
  if (!is_inf(D) || L > 0 ||
      (!is_inf(L) && !dist_has_positive_support(dist_id))) {
    real log_cdf_D;
    real log_cdf_L;

    // Get CDF at lower truncation point L
    if (is_inf(L)) {
      // No left truncation (L = -inf sentinel)
      log_cdf_L = negative_infinity();
    } else if (d == L) {
      // Reuse already computed CDF at d
      log_cdf_L = log_cdf_lower;
    } else {
      // Compute CDF at L directly
      log_cdf_L = primarycensored_lcdf(
        L | dist_id, params, pwindow,
        dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
        positive_infinity(), primary_id, primary_params
      );
    }

    // Get CDF at upper truncation point D
    if (d_upper == D) {
      log_cdf_D = log_cdf_upper;
    } else if (is_inf(D)) {
      log_cdf_D = 0;
    } else {
      log_cdf_D = primarycensored_lcdf(
        D | dist_id, params, pwindow,
        dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
        positive_infinity(), primary_id, primary_params
      );
    }

    real log_normalizer = primarycensored_log_normalizer(log_cdf_D, log_cdf_L, L);
    return log_diff_exp(log_cdf_upper, log_cdf_lower) - log_normalizer;
  } else {
    return log_diff_exp(log_cdf_upper, log_cdf_lower);
  }
}
vector primarycensored_sone_lpmf_vectorized(
  data int max_delay, data real L, data real D, data int dist_id,
  array[] real params, data real pwindow,
  data int primary_id, array[] real primary_params
) {

  int upper_interval = max_delay + 1;
  vector[upper_interval] log_pmfs;
  vector[upper_interval] log_cdfs;
  real log_normalizer;

  // Check if D is at least max_delay + 1
  if (D < upper_interval) {
    reject("D must be at least max_delay + 1");
  }

  // Compute log CDFs (without truncation normalization). The internal lower
  // bound below is 0 for positive-support delays and -inf otherwise; it is
  // inlined rather than bound to a local so Stan's type checker treats it as
  // data-only.
  // Start from max(1, floor(L)) to avoid computing unused CDFs when L > 0;
  // for L <= 0 (including -inf) start at 1 since F(d) = 0 for d <= 0.
  int start_idx = (!is_inf(L) && L > 0) ? max(1, to_int(floor(L))) : 1;
  for (d in start_idx:upper_interval) {
    log_cdfs[d] = primarycensored_lcdf(
      d | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    );
  }

  // Get CDF at lower truncation point L
  real log_cdf_L;
  if (is_inf(L)) {
    // No left truncation (L = -inf sentinel)
    log_cdf_L = negative_infinity();
  } else if (L >= 1 && L <= upper_interval && floor(L) == L) {
    // L is a positive integer within computed range, reuse cached value
    log_cdf_L = log_cdfs[to_int(L)];
  } else {
    // L is outside computed range or non-integer, compute directly
    log_cdf_L = primarycensored_lcdf(
      L | dist_id, params, pwindow,
      dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
      positive_infinity(), primary_id, primary_params
    );
  }

  // Compute log normalizer: log(F(D) - F(L))
  real log_cdf_D;
  if (D > upper_interval) {
    if (is_inf(D)) {
      log_cdf_D = 0; // log(1) = 0 for infinite D
    } else {
      log_cdf_D = primarycensored_lcdf(
        D | dist_id, params, pwindow,
        dist_has_positive_support(dist_id) ? 0.0 : negative_infinity(),
        positive_infinity(), primary_id, primary_params
      );
    }
  } else {
    log_cdf_D = log_cdfs[upper_interval];
  }

  log_normalizer = primarycensored_log_normalizer(log_cdf_D, log_cdf_L, L);

  // Compute log PMFs: log((F(d) - F(d-1)) / (F(D) - F(L)))
  for (d in 1:upper_interval) {
    if (d <= L) {
      // Delay interval [d-1, d) is entirely at or below L
      log_pmfs[d] = negative_infinity();
    } else if (d - 1 < L) {
      // L falls within interval [d-1, d), so compute mass in [L, d)
      log_pmfs[d] = log_diff_exp(log_cdfs[d], log_cdf_L) - log_normalizer;
    } else if (d == 1 && dist_has_positive_support(dist_id)) {
      // First interval [0, 1) with L <= 0 and positive-support delay:
      // F(0) = 0, so PMF = F(1) / normalizer
      log_pmfs[d] = log_cdfs[d] - log_normalizer;
    } else if (d == 1) {
      // First interval [0, 1) with L <= 0 and real-support delay: F(0) is
      // non-zero in general, so compute it explicitly.
      real log_cdf_0 = primarycensored_lcdf(
        0.0 | dist_id, params, pwindow,
        negative_infinity(), positive_infinity(),
        primary_id, primary_params
      );
      log_pmfs[d] = log_diff_exp(log_cdfs[d], log_cdf_0) - log_normalizer;
    } else {
      // Standard case: PMF = (F(d) - F(d-1)) / normalizer
      log_pmfs[d] = log_diff_exp(log_cdfs[d], log_cdfs[d-1]) - log_normalizer;
    }
  }

  return log_pmfs;
}
vector primarycensored_sone_pmf_vectorized(
  data int max_delay, data real L, data real D, data int dist_id,
  array[] real params, data real pwindow,
  data int primary_id,
  array[] real primary_params
) {
  return exp(
    primarycensored_sone_lpmf_vectorized(
      max_delay, L, D, dist_id, params, pwindow, primary_id, primary_params
    )
  );
}
/**
 * convolution_functions Functions
 *
 * This file contains functions for performing discrete convolutions, which are
 * used throughout the model to combine time series with delay distributions.
 *
 * @ingroup convolution_functions
 */

/**
 * Calculate convolution indices for the case where s <= xlen
 *
 * @param s Current position in the output vector
 * @param xlen Length of the x vector
 * @param ylen Length of the y vector
 * @return An array of integers: {start_x, end_x, start_y, end_y}
 *
 * @ingroup convolution_functions
 */
array[] int calc_conv_indices_xlen(int s, int xlen, int ylen) {
  int s_minus_ylen = s - ylen;
  int start_x = max(1, s_minus_ylen + 1);
  int end_x = s;
  int start_y = max(1, 1 - s_minus_ylen);
  int end_y = ylen;
  return {start_x, end_x, start_y, end_y};
}

/**
 * Calculate convolution indices for the case where s > xlen
 *
 * @param s Current position in the output vector
 * @param xlen Length of the x vector
 * @param ylen Length of the y vector
 * @return An array of integers: {start_x, end_x, start_y, end_y}
 *
 * @ingroup convolution_functions
 */
array[] int calc_conv_indices_len(int s, int xlen, int ylen) {
  int s_minus_ylen = s - ylen;
  int start_x = max(1, s_minus_ylen + 1);
  int end_x = xlen;
  int start_y = max(1, 1 - s_minus_ylen);
  int end_y = ylen + xlen - s;
  return {start_x, end_x, start_y, end_y};
}

/**
 * Convolve a vector with a reversed probability mass function.
 *
 * This function performs a discrete convolution of two vectors, where the second vector
 * is assumed to be an already reversed probability mass function.
 *
 * @param x The input vector to be convolved.
 * @param y The already reversed probability mass function vector.
 * @param len The desired length of the output vector.
 * @return A vector of length `len` containing the convolution result.
 * @throws If `len` is not of equal length to the sum of the lengths of `x` and `y`.
 *
 * @ingroup convolution_functions
 */
vector convolve_with_rev_pmf(vector x, vector y, int len) {
  int xlen = num_elements(x);
  int ylen = num_elements(y);

  if (xlen + ylen - 1 < len) {
    reject("convolve_with_rev_pmf: len is longer than x and y convolved");
  }

  if (xlen > len) {
    reject("convolve_with_rev_pmf: len is shorter than x");
  }

  vector[len] z;

  for (s in 1:xlen) {
    array[4] int indices = calc_conv_indices_xlen(s, xlen, ylen);
    z[s] = dot_product(x[indices[1]:indices[2]], y[indices[3]:indices[4]]);
  }

  // runs zero times unless len > xlen
  for (s in (xlen + 1):len) {
    array[4] int indices = calc_conv_indices_len(s, xlen, ylen);
    z[s] = dot_product(x[indices[1]:indices[2]], y[indices[3]:indices[4]]);
  }

  return z;
}

/**
 * Convolve infections to reported cases.
 *
 * This function convolves a vector of infections with a reversed delay
 * distribution to produce a vector of reported cases.
 *
 * @param infections A vector of infection counts.
 * @param delay_rev_pmf A vector representing the reversed probability mass
 * function of the delay distribution.
 * @param seeding_time The number of initial time steps to exclude from the
 * output.
 * @return A vector of reported cases, starting from `seeding_time + 1`.
 *
 * @ingroup convolution_functions
 */
vector convolve_to_report(vector infections,
                          vector delay_rev_pmf,
                          int seeding_time) {
  int t = num_elements(infections);
  int delays = num_elements(delay_rev_pmf);

  if (delays == 0) {
    return infections[(seeding_time + 1):t];
  }

  vector[t] unobs_reports = convolve_with_rev_pmf(infections, delay_rev_pmf, t);
  return unobs_reports[(seeding_time + 1):t];
}

/**
 * Probability Mass Function (PMF) Utilities
 *
 * This file contains functions for creating and manipulating probability mass
 * functions, particularly for discretizing continuous distributions for use in
 * delay modeling.
 *
 * @ingroup pmf_handlers
 */

/**
 * Discretise a continuous distribution using primary censoring
 *
 * Computes a properly primary-censored PMF using the vendored
 * primarycensored Stan functions. Assumes uniform primary event
 * distribution within a window of width 1.
 *
 * @param params Distribution parameters as a vector
 * @param n Number of days to calculate PMF for (max_delay + 1)
 * @param dist Distribution type using primarycensored convention
 *   (1: lognormal, 2: gamma, 3: weibull, 4: exponential)
 * @param L Left truncation point (0 for no truncation)
 * @return A vector of length n containing the discretised PMF
 *
 * @ingroup pmf_handlers
 */
vector discretised_pmf(
  vector params, data int n, int dist, data int L
) {
  int n_params = num_elements(params);
  array[n_params] real params_array;
  for (i in 1:n_params) {
    params_array[i] = params[i];
  }
  array[0] real primary_params;
  return primarycensored_sone_pmf_vectorized(
    n - 1, L * 1.0, n * 1.0, dist,
    params_array, 1.0, 1, primary_params
  );
}
/**
 * Delay Distribution Functions
 *
 * This group of functions handles the creation, manipulation, and application
 * of delay distributions in the model. Delay distributions represent the time
 * between events (e.g., infection to symptom onset, symptom onset to reporting).
 *
 */

/**
 * Get the maximum delay for each delay type
 *
 * @param delay_types Number of delay types
 * @param delay_types_p Array indicating whether each delay is parametric (1) or non-parametric (0)
 * @param delay_types_id Array mapping delay types to their respective IDs
 * @param delay_types_groups Array of indices defining groups of delay types
 * @param delay_max Array of maximum delays for parametric distributions
 * @param delay_np_pmf_groups Array of indices for accessing non-parametric PMFs
 * @return An array of maximum delays for each delay type
 *
 * @ingroup delay_handlers
 */
array[] int get_delay_type_max(
  int delay_types, array[] int delay_types_p, array[] int delay_types_id,
  array[] int delay_types_groups, array[] int delay_max,
  array[] int delay_np_pmf_groups
) {
  array[delay_types] int ret;
  for (i in 1:delay_types) {
    ret[i] = 0;
    for (j in delay_types_groups[i]:(delay_types_groups[i + 1] - 1)) {
      if (delay_types_p[j]) { // parametric
        ret[i] += delay_max[delay_types_id[j]];
      } else { // nonparametric
        ret[i] += delay_np_pmf_groups[delay_types_id[j] + 1] -
          delay_np_pmf_groups[delay_types_id[j]] - 1;
      }
    }
  }
  return ret;
}

/**
 * Get the reversed probability mass function for a delay
 *
 * @param delay_id Identifier for the specific delay distribution
 * @param len Length of the output PMF
 * @param delay_types_p Array indicating whether each delay is parametric (1) or non-parametric (0)
 * @param delay_types_id Array mapping delay types to their respective IDs
 * @param delay_types_groups Array of indices defining groups of delay types
 * @param delay_max Array of maximum delays for parametric distributions
 * @param delay_np_pmf Vector of probability mass functions for non-parametric delays
 * @param delay_np_pmf_groups Array of indices for accessing non-parametric PMFs
 * @param delay_params Vector of parameters for parametric delay distributions
 * @param delay_params_groups Array of indices for accessing delay parameters
 * @param delay_dist Array of distribution types using primarycensored
 *   convention (1: lognormal, 2: gamma, 3: weibull, 4: exponential)
 * @param left_truncate Left truncation point (0 for no truncation)
 * @param reverse_pmf Whether to reverse the PMF (1) or not (0)
 * @param cumulative Whether to return cumulative (1) or daily (0) values
 * @return A vector containing the (reversed) PMF of length len
 *
 * @ingroup delay_handlers
 */
vector get_delay_rev_pmf(
  int delay_id, int len, array[] int delay_types_p, array[] int delay_types_id,
  array[] int delay_types_groups, array[] int delay_max,
  vector delay_np_pmf, array[] int delay_np_pmf_groups,
  vector delay_params, array[] int delay_params_groups, array[] int delay_dist,
  int left_truncate, int reverse_pmf, int cumulative
) {
  // loop over delays
  vector[len] pmf = rep_vector(0, len);
  int current_len = 1;
  int new_len;
  for (i in
         delay_types_groups[delay_id]:(delay_types_groups[delay_id + 1] - 1)) {
    if (delay_types_p[i]) { // parametric
      int start = delay_params_groups[delay_types_id[i]];
      int end = delay_params_groups[delay_types_id[i] + 1] - 1;
      vector[delay_max[delay_types_id[i]] + 1] new_variable_pmf =
        discretised_pmf(
          delay_params[start:end],
          delay_max[delay_types_id[i]] + 1,
          delay_dist[delay_types_id[i]],
          0
      );
      new_len = current_len + delay_max[delay_types_id[i]];
      if (current_len == 1) { // first delay
        pmf[1:new_len] = new_variable_pmf;
      } else { // subsequent delay to be convolved
        pmf[1:new_len] = convolve_with_rev_pmf(
          pmf[1:current_len], reverse(new_variable_pmf), new_len
        );
      }
    } else { // nonparametric
      int start = delay_np_pmf_groups[delay_types_id[i]];
      int end = delay_np_pmf_groups[delay_types_id[i] + 1] - 1;
      new_len = current_len + end - start;
      if (current_len == 1) { // first delay
        pmf[1:new_len] = delay_np_pmf[start:end];
      } else { // subsequent delay to be convolved
        pmf[1:new_len] = convolve_with_rev_pmf(
          pmf[1:current_len], reverse(delay_np_pmf[start:end]), new_len
        );
      }
    }
    current_len = new_len;
  }
  if (left_truncate) {
    pmf = append_row(
      rep_vector(0, left_truncate),
      pmf[(left_truncate + 1):len] / sum(pmf[(left_truncate + 1):len])
    );
  }
  if (cumulative) {
    pmf = cumulative_sum(pmf);
  }
  if (reverse_pmf) {
    pmf = reverse(pmf);
  }
  return pmf;
}

/**
 * Update log density for delay distribution priors
 *
 * @param delay_params Vector of parameters for parametric delay distributions
 * @param delay_params_mean Vector of prior means for delay parameters
 * @param delay_params_sd Vector of prior standard deviations for delay parameters
 * @param delay_params_groups Array of indices for accessing delay parameters
 * @param delay_dist Array of distribution types using primarycensored
 *   convention (1: lognormal, 2: gamma, 3: weibull, 4: exponential)
 * @param weight Array of weights for each delay distribution in the log density
 *
 * @ingroup delay_handlers
 */
void delays_lp(vector delay_params,
               vector delay_params_mean, vector delay_params_sd,
               array[] int delay_params_groups,
               array[] int delay_dist, array[] int weight) {
  int n_delays = num_elements(delay_params_groups) - 1;
  if (n_delays == 0) {
    return;
  }
  for (d in 1:n_delays) {
    int start = delay_params_groups[d];
    int end = delay_params_groups[d + 1] - 1;
    for (s in start:end) {
      if (delay_params_sd[s] > 0) {
        if (weight[d] > 1) {
          target += weight[d] *
            normal_lpdf(
              delay_params[s] | delay_params_mean[s], delay_params_sd[s]
            );
        } else {
          delay_params[s] ~ normal(delay_params_mean[s], delay_params_sd[s]);
        }
      }
    }
  }
}

/**
 * Update log prior density for estimated nonparametric delays
 *
 * Applies Gamma(alpha, 1) priors to the raw vector that backs the
 * estimated nonparametric PMFs. Once normalised within each ragged
 * segment, this induces a Dirichlet(alpha) prior on the segment.
 * See https://mc-stan.org/docs/stan-users-guide/simplexes.html for
 * the gamma-normalisation construction.
 *
 * @param delay_np_est_raw Raw gamma-distributed values backing each
 *   estimated nonparametric PMF segment.
 * @param delay_np_est_alpha Dirichlet concentration parameters,
 *   matched element-wise to delay_np_est_raw.
 *
 * @ingroup delay_handlers
 */
void delays_np_lp(
  vector delay_np_est_raw, vector delay_np_est_alpha
) {
  if (num_elements(delay_np_est_raw) == 0) return;
  delay_np_est_raw ~ gamma(delay_np_est_alpha, 1);
}

/**
 * Combine fixed and estimated nonparametric delay PMFs
 *
 * Returns a copy of the fixed PMF vector with the estimated entries
 * overwritten by per-segment normalisation of the raw gamma values.
 * Fixed entries and structural zeros are left unchanged.
 *
 * @param delay_np_pmf Fixed PMF vector (concatenated ragged array).
 * @param delay_n_np_est Number of estimated nonparametric delays.
 * @param delay_np_est_groups Ragged-array boundaries into
 *   delay_np_est_raw (length delay_n_np_est + 1).
 * @param delay_np_est_pos For each estimated element, its position
 *   within delay_np_pmf.
 * @param delay_np_est_raw Raw gamma values backing each segment.
 * @return A vector of length num_elements(delay_np_pmf) containing
 *   the combined PMF.
 *
 * @ingroup delay_handlers
 */
vector combine_np_pmf(
  vector delay_np_pmf, int delay_n_np_est,
  array[] int delay_np_est_groups, array[] int delay_np_est_pos,
  vector delay_np_est_raw
) {
  vector[num_elements(delay_np_pmf)] ret = delay_np_pmf;
  for (i in 1:delay_n_np_est) {
    int es = delay_np_est_groups[i];
    int ee = delay_np_est_groups[i + 1] - 1;
    real seg_sum = sum(delay_np_est_raw[es:ee]);
    for (j in es:ee) {
      ret[delay_np_est_pos[j]] = delay_np_est_raw[j] / seg_sum;
    }
  }
  return ret;
}

/**
 * Generate random samples from a normal distribution with lower bounds
 *
 * @param mu Vector of means
 * @param sigma Vector of standard deviations
 * @param lb Vector of lower bounds
 * @return A vector of random samples from the truncated normal distribution
 *
 * @ingroup delay_handlers
 */
vector normal_lb_rng(vector mu, vector sigma, vector lb) {
  int len = num_elements(mu);
  vector[len] ret;
  for (i in 1:len) {
    real p = normal_cdf(lb[i] | mu[i], sigma[i]);  // cdf for bounds
    real u = uniform_rng(p, 1);
    ret[i] = (sigma[i] * inv_Phi(u)) + mu[i];  // inverse cdf for value
  }
  return ret;
}
/**
 * These functions implement approximate Gaussian processes for Stan using
 * Hilbert space methods. The functions are based on the following:
 * - https://avehtari.github.io/casestudies/Motorcycle/motorcycle_gpcourse.html (Section 4)
 * - https://doi.org/10.1007/s11222-022-10167-2
 */

/**
  * Spectral density for Exponentiated Quadratic kernel
  *
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param L Length of the interval
  * @param M Number of basis functions
  * @return A vector of spectral densities
  *
  * @ingroup estimates_smoothing
  */
vector diagSPD_EQ(real alpha, real rho, real L, int M) {
  vector[M] indices = linspaced_vector(M, 1, M);
  real factor = alpha * sqrt(sqrt(2 * pi()) * rho);
  real exponent = -0.25 * (rho * pi() / 2 / L)^2;
  return factor * exp(exponent * square(indices));
}

/**
  * Squared spectral indices shared by the Matern kernels
  *
  * Returns `square(pi() / (2 * L) * linspaced_vector(M, 1, M))`, the term that
  * appears in the denominator of every Matern spectral density. The
  * `linspaced_vector()` call uses data-only bounds and is scaled afterwards so
  * the function compiles under the data-only argument constraint in older Stan
  * versions (e.g. the one shipped with rstan).
  *
  * @param M Number of basis functions
  * @param L Length of the interval
  * @return A vector of squared spectral indices
  *
  * @ingroup estimates_smoothing
  */
vector matern_indices(int M, real L) {
  vector[M] indices = linspaced_vector(M, 1, M);
  return square(pi() / (2 * L) * indices);
}

/**
  * Spectral density for 1/2 Matern (Ornstein-Uhlenbeck) kernel
  *
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param L Length of the interval
  * @param M Number of basis functions
  * @return A vector of spectral densities
  *
  * @ingroup estimates_smoothing
  */
vector diagSPD_Matern12(real alpha, real rho, real L, int M) {
  vector[M] denom = 1 / rho + rho * matern_indices(M, L);
  return alpha * sqrt(2 ./ denom);
}

/**
  * Spectral density for 3/2 Matern kernel
  *
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param L Length of the interval
  * @param M Number of basis functions
  * @return A vector of spectral densities
  *
  * @ingroup estimates_smoothing
  */
vector diagSPD_Matern32(real alpha, real rho, real L, int M) {
  real factor = 2 * alpha * (sqrt(3) / rho)^1.5;
  vector[M] denom = 3 / square(rho) + matern_indices(M, L);
  return factor ./ denom;
}

/**
  * Spectral density for 5/2 Matern kernel
  *
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param L Length of the interval
  * @param M Number of basis functions
  * @return A vector of spectral densities
  *
  * @ingroup estimates_smoothing
  */
vector diagSPD_Matern52(real alpha, real rho, real L, int M) {
  real factor = 16 * pow(sqrt(5) / rho, 5);
  vector[M] denom = 3 * pow(5 / square(rho) + matern_indices(M, L), 3);
  return alpha * sqrt(factor ./ denom);
}

/**
  * Spectral density for periodic kernel
  *
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param M Number of basis functions
  * @return A vector of spectral densities
  *
  * @ingroup estimates_smoothing
  */
vector diagSPD_Periodic(real alpha, real rho, int M) {
  real a = inv_square(rho);
  vector[M] indices = linspaced_vector(M, 1, M);
  vector[M] q = exp(
    log(alpha) + 0.5 *
      (log(2) - a + to_vector(log_modified_bessel_first_kind(indices, a)))
  );
  return append_row(q, q);
}

/**
  * Basis functions for Gaussian Process
  *
  * @param N Number of data points
  * @param M Number of basis functions
  * @param L Length of the interval
  * @param x Vector of input data
  * @return A matrix of basis functions
  *
  * @ingroup estimates_smoothing
  */
matrix PHI(int N, int M, real L, vector x) {
  row_vector[M] k = linspaced_row_vector(M, 1, M);
  matrix[N, M] phi = sin((pi() / (2 * L) * (x + L)) * k) / sqrt(L);
  return phi;
}

/**
  * Basis functions for periodic Gaussian Process
  *
  * @param N Number of data points
  * @param M Number of basis functions
  * @param w0 Fundamental frequency
  * @param x Vector of input data
  * @return A matrix of basis functions
  *
  * @ingroup estimates_smoothing
  */
matrix PHI_periodic(int N, int M, real w0, vector x) {
  row_vector[M] k = linspaced_row_vector(M, 1, M);
  matrix[N, M] w0xk = (w0 * x) * k;
  return append_col(cos(w0xk), sin(w0xk));
}

/**
  * Setup Gaussian process noise dimensions
  *
  * @param ot_h Observation time horizon
  * @param t Total time points
  * @param horizon Forecast horizon
  * @param estimate_r Indicator if estimating r
  * @param stationary Indicator if stationary
  * @param future_fixed Indicator if future is fixed
  * @param fixed_from Fixed point from
  * @return Number of noise terms
  *
  * @ingroup estimates_smoothing
  */
int setup_noise(int ot_h, int t, int horizon, int estimate_r,
                int stationary, int future_fixed, int fixed_from) {
  int noise_time = estimate_r > 0 ? (stationary > 0 ? ot_h : ot_h - 1) : t;
  int noise_terms =
    future_fixed > 0 ? (noise_time - horizon + fixed_from) : noise_time;
  return noise_terms;
}

/**
  * Setup approximate Gaussian process
  *
  * @param M Number of basis functions
  * @param L Length of the interval
  * @param dimension Dimension of the process
  * @param is_periodic Indicator if the process is periodic
  * @param w0 Fundamental frequency for periodic process
  * @return A matrix of basis functions
  *
  * @ingroup estimates_smoothing
  */
matrix setup_gp(int M, real L, int dimension, int is_periodic, real w0) {
  vector[dimension] x = linspaced_vector(dimension, 1, dimension);
  x = 2 * (x - mean(x)) / (max(x) - 1);
  if (is_periodic) {
    return PHI_periodic(dimension, M, w0, x);
  } else {
    return PHI(dimension, M, L, x);
  }
}

/**
  * Update Gaussian process using spectral densities
  *
  * @param PHI Basis functions matrix
  * @param M Number of basis functions
  * @param L Length of the interval
  * @param alpha Scaling parameter
  * @param rho Length scale parameter
  * @param eta Vector of noise terms
  * @param type Type of kernel (0: SE, 1: Periodic, 2: Matern)
  * @param nu Smoothness parameter for Matern kernel
  * @return A vector of updated noise terms
  */
vector update_gp(matrix PHI, int M, real L, real alpha,
                 real rho, vector eta, int type, real nu) {
  vector[type == 1 ? 2 * M : M] diagSPD;    // spectral density

  // GP in noise - spectral densities
  if (type == 0) {
    diagSPD = diagSPD_EQ(alpha, rho, L, M);
  } else if (type == 1) {
    diagSPD = diagSPD_Periodic(alpha, rho, M);
  } else if (type == 2) {
    if (nu == 0.5) {
      diagSPD = diagSPD_Matern12(alpha, rho, L, M);
    } else if (nu == 1.5) {
      diagSPD = diagSPD_Matern32(alpha, rho, L, M);
    } else if (nu == 2.5) {
      diagSPD = diagSPD_Matern52(alpha, rho, L, M);
    } else {
      reject("nu must be one of 0.5, 1.5, or 2.5; found nu=", nu);
    }
  }
  return PHI * (diagSPD .* eta);
}

/**
  * Priors for Gaussian process (excluding length scale)
  *
  * @param eta Vector of noise terms
  *
  * @ingroup estimates_smoothing
  */
void gaussian_process_lp(vector eta) {
  eta ~ std_normal();
}

/**
 * Reproduction Number (Rt) Functions
 *
 * This group of functions handles the calculation, updating, and conversion of
 * reproduction numbers in the model. The reproduction number represents the average
 * number of secondary infections caused by a single infected individual.
 *
 * @ingroup rt_estimation
 */

/**
 * @ingroup rt_estimation
 * @brief Update a vector of effective reproduction numbers (Rt) based on
 * an intercept, breakpoints (i.e. a random walk), and a Gaussian
 * process.
 *
 * @param t Length of the time series
 * @param R0 Initial reproduction number
 * @param noise Vector of Gaussian process noise values
 * @param bps Array of breakpoint indices
 * @param bp_effects Vector of breakpoint effects
 * @param stationary Whether the Gaussian process is stationary (1) or non-stationary (0)
 * @param n_centre Number of leading positions over which to centre the
 *   non-stationary GP trajectory and the breakpoint random walk. Set to the
 *   observation window length so the centring is invariant to the forecast
 *   horizon. Ignored for the GP branch when `stationary` is 1; the breakpoint
 *   path is centred whenever breakpoints are present.
 * @return A vector of length t containing the updated Rt values
 */
vector update_Rt(int t, real R0, vector noise, array[] int bps,
                 vector bp_effects, int stationary, int n_centre) {
  // define control parameters
  int bp_n = num_elements(bp_effects);
  int gp_n = num_elements(noise);
  // initialise intercept
  vector[t] logR = rep_vector(log(R0), t);
  //initialise breakpoints + rw
  if (bp_n) {
    vector[bp_n + 1] bp0;
    bp0[1] = 0;
    bp0[2:(bp_n + 1)] = cumulative_sum(bp_effects);
    vector[t] bp = bp0[bps];
    // Centre over the observation window (same identifiability fix as the GP below).
    bp -= mean(bp[1:n_centre]);
    logR = logR + bp;
  }
  //initialise gaussian process
  if (gp_n) {
    vector[t] gp = rep_vector(0, t);
    if (stationary) {
      gp[1:gp_n] = noise;
      // fix future gp based on last estimated
      if (t > gp_n) {
        gp[(gp_n + 1):t] = rep_vector(noise[gp_n], t - gp_n);
      }
    } else {
      gp[2:(gp_n + 1)] = noise;
      gp = cumulative_sum(gp);
      // Centre over the observation window (same identifiability fix as the BP above).
      gp -= mean(gp[1:n_centre]);
    }
    logR = logR + gp;
  }

  return exp(logR);
}

/**
 * Calculate the log-probability of the reproduction number (Rt) priors
 *
 * This function adds the log density contributions from priors on initial infections
 * and breakpoint effects to the target.
 *
 * @param initial_infections_scale Array of initial infection values
 * @param bp_effects Vector of breakpoint effects
 * @param bp_sd Array of breakpoint standard deviations
 * @param bp_n Number of breakpoints
 * @param cases Array of observed case counts
 * @param initial_infections_guess Initial guess for infections based on cases
 *
 * @ingroup rt_estimation
 */
void rt_lp(array[] real initial_infections_scale, vector bp_effects,
           array[] real bp_sd, int bp_n, array[] int cases,
           real initial_infections_guess) {
  //breakpoint effects on Rt
  if (bp_n > 0) {
    bp_sd[1] ~ normal(0, 0.1) T[0,];
    bp_effects ~ normal(0, bp_sd[1]);
  }
  initial_infections_scale ~ normal(initial_infections_guess, 2);
}

/**
 * Helper function for calculating r from R using Newton's method
 *
 * This function performs a single Newton step in the iterative calculation
 * of the growth rate r from the reproduction number R.
 *
 * Code is based on Julia code from
 * https://github.com/CDCgov/Rt-without-renewal/blob/d6344cc6e451e3e6c4188e4984247f890ae60795/EpiAware/test/predictive_checking/fast_approx_for_r.jl
 * under Apache license 2.0.
 *
 * @param R Reproduction number
 * @param r Current estimate of the growth rate
 * @param pmf Generation time probability mass function (first index: 0)
 * @return The Newton step for updating r
 *
 * @ingroup rt_estimation
 */
real R_to_r_newton_step(real R, real r, vector pmf) {
  int len = num_elements(pmf);
  vector[len] zero_series = linspaced_vector(len, 0, len - 1);
  vector[len] exp_r = exp(-r * zero_series);
  real ret = (R * dot_product(pmf, exp_r) - 1) /
    (- R * dot_product(pmf .* zero_series, exp_r));
  return(ret);
}

/**
 * Estimate the growth rate r from reproduction number R
 *
 * This function uses the Newton method to solve for the growth rate r
 * that corresponds to a given reproduction number R, using the generation
 * time distribution.
 *
 * Code is based on Julia code from
 * https://github.com/CDCgov/Rt-without-renewal/blob/d6344cc6e451e3e6c4188e4984247f890ae60795/EpiAware/test/predictive_checking/fast_approx_for_r.jl
 * under Apache license 2.0.
 *
 * @param R Reproduction number
 * @param gt_rev_pmf Reversed probability mass function of the generation time
 * @param abs_tol Absolute tolerance for the Newton solver
 * @return The estimated growth rate r
 *
 * @ingroup rt_estimation
 */
real R_to_r(real R, vector gt_rev_pmf, real abs_tol) {
  int gt_len = num_elements(gt_rev_pmf);
  vector[gt_len] gt_pmf = reverse(gt_rev_pmf);
  real mean_gt = dot_product(gt_pmf, linspaced_vector(gt_len, 0, gt_len - 1));
  real r = fmax((R - 1) / (R * mean_gt), -1);
  real step = abs_tol + 1;
  while (abs(step) > abs_tol) {
    step = R_to_r_newton_step(R, r, gt_pmf);
    r -= step;
  }

  return(r);
}
/**
 * Infection Modeling Functions
 *
 * This group of functions handles the generation, calculation, and backcalculation
 * of infection time series in the model. These functions implement the core
 * epidemiological dynamics, including the renewal equation approach.
 *
 * @ingroup infections_estimation
 */

/**
 * Calculate infectiousness for a single time point
 *
 * This function computes the weighted sum of past infections with the generation
 * time distribution to determine the current infectiousness.
 *
 * @param infections Vector of infection counts
 * @param gt_rev_pmf Vector of reversed generation time PMF
 * @param seeding_time Number of time steps used for seeding
 * @param index Current time index (relative to seeding_time)
 * @return The infectiousness at the specified time point
 *
 * @ingroup infections_estimation
 */
real update_infectiousness(vector infections, vector gt_rev_pmf,
                           int seeding_time, int index){
  int gt_length = num_elements(gt_rev_pmf);
  // work out where to start the convolution of past infections with the
  // generation time distribution: (current_time - maximal generation time) if
  // that is >= 1, otherwise 1
  int inf_start = max(1, (index + seeding_time - gt_length + 1));
  // work out where to end the convolution: current_time
  int inf_end = (index + seeding_time);
  // number of indices of the generation time to sum over
  // (inf_end - inf_start + 1)
  int pmf_accessed = min(gt_length, index + seeding_time);
  // calculate the elements of the convolution
  real new_inf = dot_product(
    infections[inf_start:inf_end], tail(gt_rev_pmf, pmf_accessed)
  );
  return(new_inf);
}

/**
 * @ingroup infections_estimation
 * @brief Generate infections using a renewal equation approach
 *
 * This function implements the renewal equation to generate a time series of
 * infections based on reproduction numbers and the generation time distribution.
 * It can also account for population depletion if a population size is specified.
 *
 * @param R Vector of reproduction numbers
 * @param uot Unobserved time (seeding time)
 * @param gt_rev_pmf Vector of reversed generation time PMF
 * @param initial_infections Array of initial infection values
 * @param pop Initial susceptible population (0 for unlimited)
 * @param use_pop Population adjustment mode (0=none, 1=forecast only, 2=all)
 * @param pop_floor Minimum susceptible population (floor to prevent instability)
 * @param ht Horizon time
 * @param obs_scale Whether to scale by fraction observed (1) or not (0)
 * @param fraction_observed Fraction of infections that are observed
 * @param initial_as_scale Whether initial infections are a scaling factor (1) or not (0)
 * @return A vector of infection counts
 */
vector generate_infections(vector R, int uot, vector gt_rev_pmf,
                           array[] real initial_infections, real pop,
                           int use_pop, real pop_floor, int ht, int obs_scale, real fraction_observed,
                           int initial_as_scale) {
  // time indices and storage
  int ot = num_elements(R);
  int nht = ot - ht;
  int t = ot + uot;
  real exp_adj_Rt;
  vector[t] infections = rep_vector(0, t);
  vector[ot] cum_infections;
  vector[ot] infectiousness;
  real growth = R_to_r(R[1], gt_rev_pmf, 1e-3);
  // Initialise infections using daily growth
  if (initial_as_scale) {
    infections[1] = exp(initial_infections[1] - growth * uot);
    if (obs_scale) {
      infections[1] = infections[1] / fraction_observed;
    }
  } else {
    infections[1] = exp(initial_infections[1]);
  }
  if (uot > 1) {
    real exp_growth = exp(growth);
    for (s in 2:uot) {
      infections[s] = infections[s - 1] * exp_growth;
    }
  }
  // calculate cumulative infections
  if (use_pop) {
    cum_infections[1] = sum(infections[1:uot]);
  }
  // iteratively update infections
  for (s in 1:ot) {
    infectiousness[s] = update_infectiousness(infections, gt_rev_pmf, uot, s);
    if ((use_pop == 1 && s > nht) || use_pop == 2) {
      real susceptible = fmax(pop_floor, pop - cum_infections[s]);
      exp_adj_Rt = exp(-R[s] * infectiousness[s] / susceptible);
      infections[s + uot] = susceptible * fmax(0, 1 - exp_adj_Rt);
    } else{
      infections[s + uot] = R[s] * infectiousness[s];
    }
    if (use_pop && s < ot) {
      cum_infections[s + 1] = cum_infections[s] + infections[s + uot];
    }
  }
  return(infections);
}

/**
 * Backcalculate infections from cases
 *
 * This function estimates infections by working backwards from observed cases,
 * applying noise to account for uncertainty in the process.
 *
 * @param shifted_cases Vector of shifted case counts
 * @param noise Vector of noise values
 * @param fixed Whether to use fixed (1) or variable (0) noise
 * @param prior Prior type to use (0: noise only, 1: cases * noise, 2: random walk)
 * @return A vector of infection counts
 *
 * @ingroup infections_estimation
 */
vector deconvolve_infections(vector shifted_cases, vector noise, int fixed,
                             int prior) {
  int t = num_elements(shifted_cases);
  vector[t] infections = rep_vector(1e-5, t);
  if (!fixed) {
    vector[t] exp_noise = exp(noise);
    if (prior == 1) {
      infections = infections + shifted_cases .* exp_noise;
    } else if (prior == 0) {
     infections = infections + exp_noise;
    } else if (prior == 2) {
      infections[1] = infections[1] + shifted_cases[1] * exp_noise[1];
      for (i in 2:t) {
        infections[i] = infections[i - 1] * exp_noise[i];
      }
    }
  } else {
    infections = infections + shifted_cases;
  }
  return(infections);
}
/**
 * Apply day of the week effect to reports
 *
 * This function applies a day of the week effect to a vector of reports.
 *
 * @param reports Vector of reports to be adjusted.
 * @param day_of_week Array of integers representing the day of the week for
 * each report.
 * @param effect Vector of day of week effects.
 *
 * @return A vector of reports adjusted for day of the week effects.
 *
 * @ingroup observation_model
 */
vector day_of_week_effect(vector reports, array[] int day_of_week,
                          vector effect) {
  int wl = num_elements(effect);
  vector[wl] scaled_effect = wl * effect;
  return reports .* scaled_effect[day_of_week];
}

/**
 * Scale observations by fraction reported
 *
 * This function scales a vector of reports by a fraction observed.
 *
 * @param reports Vector of reports to be scaled.
 * @param fraction_observed Real value representing the fraction observed.
 *
 * @return A vector of scaled reports.
 *
 * @ingroup observation_model
 */
vector scale_obs(vector reports, real fraction_observed) {
  int t = num_elements(reports);
  vector[t] scaled_reports;
  scaled_reports = reports * fraction_observed;
  return(scaled_reports);
}

/**
 * Truncate observed data by a truncation distribution
 *
 * This function truncates a vector of reports based on a truncation
 * distribution.
 *
 * @param reports Vector of reports to be truncated.
 * @param trunc_rev_cmf Vector representing the reverse cumulative mass function
 * of the truncation distribution.
 * @param reconstruct Integer flag indicating whether to reconstruct (1) or
 * truncate (0) the data.
 *
 * @return A vector of truncated reports.
 *
 * @ingroup observation_model
 */
vector truncate_obs(vector reports, vector trunc_rev_cmf, int reconstruct) {
  int t = num_elements(reports);
  int trunc_max = num_elements(trunc_rev_cmf);
  vector[t] trunc_reports = reports;
  // Calculate cmf of truncation delay
  int joint_max = min(t, trunc_max);
  int first_t = t - joint_max + 1;
  int first_trunc = trunc_max - joint_max + 1;

  // Apply cdf of truncation delay to truncation max last entries in reports
  if (reconstruct) {
    trunc_reports[first_t:t] ./= trunc_rev_cmf[first_trunc:trunc_max];
  } else {
    trunc_reports[first_t:t] .*= trunc_rev_cmf[first_trunc:trunc_max];
  }
  return(trunc_reports);
}

/**
 * Update log density for truncation distribution priors
 *
 * This function updates the log density for truncation distribution priors.
 *
 * @param truncation_mean Array of real values for truncation mean.
 * @param truncation_sd Array of real values for truncation standard deviation.
 * @param trunc_mean_mean Array of real values for mean of truncation mean
 * prior.
 * @param trunc_mean_sd Array of real values for standard deviation of
 * truncation mean prior.
 * @param trunc_sd_mean Array of real values for mean of truncation standard
 * deviation prior.
 * @param trunc_sd_sd Array of real values for standard deviation of truncation
 * standard deviation prior.
 *
 * @ingroup observation_model
 */
void truncation_lp(array[] real truncation_mean, array[] real truncation_sd,
                   array[] real trunc_mean_mean, array[] real trunc_mean_sd,
                   array[] real trunc_sd_mean, array[] real trunc_sd_sd) {
  int truncation = num_elements(truncation_mean);
  if (truncation) {
    if (trunc_mean_sd[1] > 0) {
      // uncertain mean
      truncation_mean ~ normal(trunc_mean_mean, trunc_mean_sd);
    }
    if (trunc_sd_sd[1] > 0) {
      // uncertain sd
      truncation_sd ~ normal(trunc_sd_mean, trunc_sd_sd);
    }
  }
}

/**
 * Negative binomial overdispersion for the reporting model
 *
 * Converts the reporting overdispersion parameter into the `phi` of the
 * negative binomial. When no overdispersion is modelled a large value is
 * returned so the negative binomial behaves like a Poisson.
 *
 * @param reporting_overdispersion Real value for reporting overdispersion.
 * @param model_type Integer indicating the model type (0 for Poisson, >0 for
 * Negative Binomial).
 *
 * @return The negative binomial overdispersion `phi`.
 *
 * @ingroup observation_model
 */
real reporting_phi(real reporting_overdispersion, int model_type) {
  return model_type ? inv_square(reporting_overdispersion) : 1e5;
}

/**
 * Update log density for reported cases
 *
 * This function updates the log density for reported cases based on the
 * specified model type.
 *
 * @param cases Array of integer observed cases.
 * @param case_times Array of integer time indices for observed cases.
 * @param reports Vector of expected reports.
 * @param reporting_overdispersion Real values for reporting overdispersion.
 * @param model_type Integer indicating the model type (0 for Poisson, >0 for
 * Negative Binomial).
 * @param weight Real value for weighting the log density contribution.
 *
 * @ingroup observation_model
 */
void report_lp(array[] int cases, array[] int case_times, vector reports,
               real reporting_overdispersion, int model_type, real weight) {
  int n = num_elements(case_times); // number of observations
  vector[n] obs_reports = reports[case_times]; // reports at observation time
  if (model_type) {
    real phi = reporting_phi(reporting_overdispersion, model_type);
    if (weight == 1) {
      cases ~ neg_binomial_2(obs_reports, phi);
    } else {
      target += neg_binomial_2_lpmf(
        cases | obs_reports, phi
      ) * weight;
    }
  } else {
    if (weight == 1) {
      cases ~ poisson(obs_reports);
    } else {
      target += poisson_lpmf(cases | obs_reports) * weight;
    }
  }
}

/**
 * Accumulate reports according to a binary flag at each time point
 *
 * This function accumulates reports according to a binary flag at each time
 * point.
 *
 * @param reports Vector of expected reports.
 * @param accumulate Array of integers indicating, for each time point, whether
 * to accumulate or not.
 *
 * @return A vector of accumulated reports.
 *
 * @ingroup observation_model
 */
vector accumulate_reports(vector reports, array[] int accumulate) {
  int ot_h = num_elements(reports); // number of reporting time points modelled
  vector[ot_h] accumulated_reports = reports;
  for (i in 1:(ot_h - 1)) {
    if (accumulate[i]) { // first observation gets ignored when accumulating
      accumulated_reports[i + 1] += accumulated_reports[i];
    }
  }
  return accumulated_reports;
}

/**
 * Calculate log likelihood for reported cases
 *
 * This function calculates the log likelihood for reported cases based on the
 * specified model type.
 *
 * @param cases Array of integer observed cases.
 * @param reports Vector of expected reports.
 * @param reporting_overdispersion Array of real values for reporting overdispersion.
 * @param model_type Integer indicating the model type (0 for Poisson, >0 for
 * Negative Binomial).
 * @param weight Real value for weighting the log likelihood contribution.
 *
 * @return A vector of log likelihoods for each time point.
 *
 * @ingroup observation_model
 */
vector report_log_lik(array[] int cases, vector reports,
                      real reporting_overdispersion, int model_type, real weight) {
  int t = num_elements(reports);
  vector[t] log_lik;

  // defer to poisson if phi is large, to avoid overflow
  if (model_type == 0) {
    for (i in 1:t) {
      log_lik[i] = poisson_lpmf(cases[i] | reports[i]) * weight;
    }
  } else {
    real phi = reporting_phi(reporting_overdispersion, model_type);
    for (i in 1:t) {
      log_lik[i] = neg_binomial_2_lpmf(
        cases[i] | reports[i], phi
      ) * weight;
    }
  }
  return(log_lik);
}


/**
 * Custom safe version of the negative binomial sampler
 *
 * This function generates random samples of the negative binomial distribution
 * whilst avoiding numerical overflows. In particular:
 * - if the mu parameter is very small it always returns 0
 * - if the phi parameter is large it returns a sample from a Poisosn
 *   distribution
 * - if the gamma rate of the gamma-Poisson mixture used for simulating from the
 *   distribution is very large, it returns 1e8
 * - in all other cases it returns a sample from the negative binomial
 *   distribution
 *
 * @param mu Real value for mean mu.
 * @param phi Real value for phi.
 *
 * @return A random sample
 *
 * @ingroup handlers_and_helpers
 */
int neg_binomial_2_safe_rng(real mu, real phi) {
  if (mu < 1e-8) {
    return(0);
  } else if (phi > 1e4) {
    return(poisson_rng(mu > 1e8 ? 1e8 : mu));
  } else {
    real gamma_rate = gamma_rng(phi, phi / mu);
    return(poisson_rng(gamma_rate > 1e8 ? 1e8 : gamma_rate));
  }
}

/**
 * Generate random samples of reported cases
 *
 * This function generates random samples of reported cases based on the
 * specified model type.
 *
 * @param reports Vector of expected reports.
 * @param reporting_overdispersion Real value for reporting overdispersion.
 * @param model_type Integer indicating the model type (0 for Poisson, >0 for
 * Negative Binomial).
 *
 * @return An array of integer sampled reports.
 *
 * @ingroup observation_model
 */
array[] int report_rng(vector reports, real reporting_overdispersion, int model_type) {
  int t = num_elements(reports);
  array[t] int sampled_reports;
  real phi = reporting_phi(reporting_overdispersion, model_type);

  for (s in 1:t) {
    sampled_reports[s] = neg_binomial_2_safe_rng(reports[s], phi);
  }
  return(sampled_reports);
}
/**
 * Generated Quantities Functions
 *
 * Functions for calculating additional quantities from model outputs.
 */

/**
 * Calculate Rt directly from inferred infections
 *
 * This function estimates the reproduction number (Rt) using the Cori et al. approach,
 * directly from a time series of infections. Optionally applies smoothing.
 *
 * @param infections Vector of infection counts
 * @param seeding_time Number of time steps used for seeding
 * @param gt_rev_pmf Vector of reversed generation time PMF
 * @param smooth Number of time steps to use for smoothing (0 for no smoothing)
 * @return A vector of reproduction numbers (Rt)
 *
 * @ingroup rt_estimation
 */
vector calculate_Rt(vector infections, int seeding_time,
                    vector gt_rev_pmf, int smooth) {
  int t = num_elements(infections);
  int ot = t - seeding_time;
  vector[ot] R;
  vector[ot] sR;
  vector[ot] infectiousness = rep_vector(1e-5, ot);
  // calculate Rt using Cori et al. approach
  for (s in 1:ot) {
    infectiousness[s] += update_infectiousness(
      infections, gt_rev_pmf, seeding_time, s
    );
    R[s] = infections[s + seeding_time] / infectiousness[s];
  }
  if (smooth) {
    for (s in 1:ot) {
      real window = 0;
      sR[s] = 0;
      for (i in max(1, s - smooth):min(ot, s + smooth)) {
        sR[s] += R[i];
        window += 1;
      }
      sR[s] = sR[s] / window;
    }
  } else{
    sR = R;
  }
  return(sR);
}

/**
 * Calculate growth rate
 *
 * This function calculates the growth rate from a time series of infections
 * by taking the log difference between consecutive time points.
 *
 * @param infections Vector of infection counts
 * @param seeding_time Number of time steps used for seeding
 * @param gt_rev_pmf Vector of reversed generation time PMF
 * @param growth_method Either 0 (log derivative of new infections) or 1 (log 
 * derivative of infectiousness, see Parag et al. 2022)
 *
 * @return A vector of growth rates
 *
 * @ingroup rt_estimation
 */
vector calculate_growth(vector infections, int seeding_time, 
                        data vector gt_rev_pmf, int growth_method) {
  if (growth_method == 0) {
    return(calculate_growth_infections(infections, seeding_time));
  } else if (growth_method == 1) {
    return(calculate_growth_infness(infections, seeding_time, gt_rev_pmf));
  } else {
    reject("growth_method must be 0 (infections) or 1 (infectiousness).");
  }
}

/**
 * Calculate growth rate
 *
 * This function calculates the growth rate from a time series of infections
 * by taking the log difference between consecutive time points.
 *
 * @param infections Vector of infection counts
 * @param seeding_time Number of time steps used for seeding
 *
 * @return A vector of growth rates
 *
 * @ingroup rt_estimation
 */
vector calculate_growth_infections(vector infections, int seeding_time) {
  int t = num_elements(infections);
  int ot = t - seeding_time;
  int start = 1 + seeding_time;
  vector[t] log_inf = log(infections);
  vector[ot - 1] growth = log_inf[(1+start):t] - log_inf[start:(t - 1)];
  return(growth);
}

/**
 * Calculate growth rate using approach by Parag et al. 2022 
 * (https://doi.org/10.1111/rssa.12867)
 *
 * This function calculates the growth rate from a time series of infections
 * by taking the log derivative on the infectiousness and shifting it by the 
 * mean generation time.
 *
 * @param infections Vector of infection counts
 * @param seeding_time Number of time steps used for seeding
 * @param gt_rev_pmf Vector of reversed generation time PMF
 *  
 * @return A vector of growth rates
 *
 * @ingroup rt_estimation
 */
vector calculate_growth_infness(vector infections, int seeding_time, 
                                       data vector gt_rev_pmf) {
  int t = num_elements(infections);
  int ot = t - seeding_time;
  int start = 1 + seeding_time;
  if (ot <= 1) {
    reject("seeding_time must >1 time step shorter than infections vector.");
    }
  // infectiousness
  vector[ot] infness_log = rep_vector(1e-5, ot);
  for (s in 1:ot) {
    infness_log[s] += log(update_infectiousness(
      infections, gt_rev_pmf, seeding_time, s
    ));
  }
  // mean generation time, will always be >= 1
  int gt_length = num_elements(gt_rev_pmf);
  int mean_gen = to_int(round( // round weighted mean to nearest int
    dot_product(reverse(linspaced_vector(gt_length, 1, gt_length)), gt_rev_pmf)
    ));
  // growth rate
  vector[ot - 1] growth = infness_log[2:ot] - infness_log[1:(ot - 1)];  
  // shift by mean_gen (most recent growth rates remain undefined)
  growth[1:(ot - 1 - mean_gen)] = growth[(1 + mean_gen):(ot - 1)];
  growth[(ot - mean_gen):(ot - 1)] = rep_vector(not_a_number(), mean_gen);
  return(growth);
}
/**
 * Parameter Handlers
 *
 * This group of functions handles parameter access, retrieval, and prior
 * specification in the model. Parameters can be either fixed (specified in advance)
 * or variable (estimated during inference).
 */

/**
 * Get a parameter value from either fixed or variable parameters
 *
 * This function retrieves a parameter value based on its ID, checking first if it's
 * a fixed parameter and then if it's a variable parameter.
 *
 * @param id Parameter ID
 * @param params_fixed_lookup Array of fixed parameter lookup indices
 * @param params_variable_lookup Array of variable parameter lookup indices
 * @param params_value Vector of fixed parameter values
 * @param params Vector of variable parameter values
 * @return The parameter value (scalar)
 *
 * @ingroup parameter_handlers
 */
real get_param(int id,
               array[] int params_fixed_lookup,
               array[] int params_variable_lookup,
               vector params_value, vector params) {
  if (id == 0) {
    return 0; // parameter not used
  } else if (params_fixed_lookup[id]) {
    return params_value[params_fixed_lookup[id]];
  } else {
    return params[params_variable_lookup[id]];
  }
}

/**
 * Get a parameter value from either fixed or variable parameters (matrix version)
 *
 * This function is an overloaded version of get_param that works with a matrix of
 * parameter values, returning a vector of parameter values for multiple samples.
 *
 * @param id Parameter ID
 * @param params_fixed_lookup Array of fixed parameter lookup indices
 * @param params_variable_lookup Array of variable parameter lookup indices
 * @param params_value Vector of fixed parameter values
 * @param params Matrix of variable parameter values (rows are samples)
 * @return A vector of parameter values across samples
 *
 * @ingroup parameter_handlers
 */
vector get_param(int id,
                 array[] int params_fixed_lookup,
                 array[] int params_variable_lookup,
                 vector params_value, matrix params) {
  int n_samples = rows(params);
  if (id == 0) {
    return rep_vector(0, n_samples) ; // parameter not used
  } else if (params_fixed_lookup[id]) {
    return rep_vector(params_value[params_fixed_lookup[id]], n_samples);
  } else {
    return params[, params_variable_lookup[id]];
  }
}

/**
 * Apply a truncated prior to a value
 *
 * Adds the log density of the chosen distribution, truncated to `[lb, ub]`,
 * to the target.
 *
 * @param value Value to apply the prior to (sampled parameter or derived
 *   quantity).
 * @param dist Prior distribution type (0: lognormal, 1: gamma, 2: normal).
 * @param p1 First distribution parameter.
 * @param p2 Second distribution parameter.
 * @param lb Lower bound of the parameter's support.
 * @param ub Upper bound of the parameter's support.
 *
 * @ingroup parameter_handlers
 */
void apply_prior_lp(real value, int dist,
                    real p1, real p2,
                    real lb, real ub) {
  if (dist == 0) {
    value ~ lognormal(p1, p2) T[lb, ub];
  } else if (dist == 1) {
    value ~ gamma(p1, p2) T[lb, ub];
  } else if (dist == 2) {
    value ~ normal(p1, p2) T[lb, ub];
  } else {
    reject("dist must be <= 2");
  }
}

/**
 * Update log density for parameter priors
 *
 * Adds the log density contributions from parameter priors to the target.
 *
 * @param params Vector of parameter values
 * @param prior_dist Array of prior distribution types (0: lognormal, 1: gamma, 2: normal)
 * @param prior_dist_params Vector of prior distribution parameters
 * @param params_lower Vector of lower bounds for parameters
 * @param params_upper Vector of upper bounds for parameters
 *
 * @ingroup parameter_handlers
 */
void params_lp(vector params, array[] int prior_dist,
              vector prior_dist_params, vector params_lower,
              vector params_upper) {
  int params_id = 1;
  int num_params = num_elements(params);
  for (id in 1:num_params) {
    apply_prior_lp(
      params[id], prior_dist[id],
      prior_dist_params[params_id], prior_dist_params[params_id + 1],
      params_lower[id], params_upper[id]
    );
    params_id += 2;
  }
}

/**
 * Apply user priors on the initial values of centred-GP-wrapped trajectories
 *
 * For each registered init prior, dispatches on the parameter id to the
 * corresponding derived initial value, applies the user's truncated prior
 * via `apply_prior_lp`, and adds the natural-to-log Jacobian (the shift
 * from sampled log-mean to derived log initial value contributes nothing,
 * as its Jacobian determinant is one).
 *
 * @param init_param_ids Per-prior id of the parameter the prior applies to.
 * @param init_dists Per-prior distribution code (0: lognormal, 1: gamma,
 *   2: normal).
 * @param init_dist_params Flat ragged vector of distribution parameters,
 *   two per prior.
 * @param init_lower Per-prior lower bound on the parameter's support.
 * @param init_upper Per-prior upper bound on the parameter's support.
 * @param param_id_R0 Registered id of R0.
 * @param R Reproduction-number trajectory.
 *
 * @ingroup parameter_handlers
 */
void init_priors_lp(array[] int init_param_ids, array[] int init_dists,
                    vector init_dist_params,
                    vector init_lower, vector init_upper,
                    int param_id_R0, vector R) {
  int params_id = 1;
  for (i in 1:num_elements(init_param_ids)) {
    real init_value;
    if (init_param_ids[i] == param_id_R0) {
      init_value = R[1];
    } else {
      reject("no init param registered for id ", init_param_ids[i]);
    }
    apply_prior_lp(
      init_value, init_dists[i],
      init_dist_params[params_id], init_dist_params[params_id + 1],
      init_lower[i], init_upper[i]
    );
    target += log(init_value);
    params_id += 2;
  }
}


}

data {
int t; // unobserved time
int lt; // timepoints in the likelihood
int it; // imputed time points
int seeding_time; // time period used for seeding and not observed
int horizon; // forecast horizon
int future_time; // time in future for Rt
array[lt] int<lower = 0> cases; // observed cases
array[lt] int case_times; // time of observed cases
array[it] int imputed_times; // time of imputed cases
int any_accumulate; // Should any missing values be accumulated?
// Should missing values be accumulated (by time)
array[t - seeding_time] int accumulate;
int<lower = 0> delay_n; // number of delay distributions
int<lower = 0> delay_n_p; // number of parametric delay distributions
int<lower = 0> delay_n_np; // number of nonparametric delay distributions
array[delay_n_p] int<lower = 1> delay_max; // maximum delay distribution
// primarycensored dist_id: 1 = lognormal; 2 = gamma; 3 = weibull
array[delay_n_p] int<lower = 1> delay_dist;

int<lower = 0> delay_np_pmf_length; // number of nonparametric pmf elements
// ragged array of fixed PMFs
vector<lower = 0, upper = 1>[delay_np_pmf_length] delay_np_pmf;
// links to ragged array
array[delay_n_np + 1] int<lower = 1> delay_np_pmf_groups;

// Estimated nonparametric delays (Dirichlet via gamma trick)
int<lower = 0> delay_n_np_est; // number of estimated NP delays
int<lower = 0> delay_np_est_length; // total estimated elements
array[delay_n_np_est + 1] int delay_np_est_groups; // ragged indexing
vector<lower = 0>[delay_np_est_length] delay_np_est_alpha; // Dirichlet alpha
// position of each estimated element within delay_np_pmf
array[delay_np_est_length] int delay_np_est_pos;
// maps estimated NP delay index to its position in delay_np_pmf
array[delay_n_np_est] int delay_np_est_which;

// number of parameters across all parametric delay distributions
int<lower = 0> delay_params_length;
// ragged array of lower bounds of the parameters
vector[delay_params_length] delay_params_lower;
// ragged array of mean parameters for parametric delay distributions
vector<lower = delay_params_lower>[delay_params_length] delay_params_mean;
// ragged array of sd of parameters for parametric delay distributions
vector<lower = 0>[delay_params_length] delay_params_sd;
// links to ragged array
array[delay_n_p + 1] int<lower = 0> delay_params_groups;

array[delay_n_p] int<lower = 0> delay_weight; // delay weights
int<lower = 0> delay_types; // number of delay types
// whether delay types are parametric
array[delay_n] int<lower = 0> delay_types_p;
// whether delay types are parametric
array[delay_n] int<lower = 0> delay_types_id;
// index of each delay (parametric or non)
array[delay_types + 1] int<lower = 0> delay_types_groups;
real L; // boundary value for infections gp
int<lower = 1> M; // basis functions for infections gp
int gp_type;
real nu; // smoothness parameter for Matern kernel (used if gp_type = 2)
real w0; // fundamental frequency for periodic kernel (used if gp_type = 1)
int stationary; // is underlying gaussian process first or second order
int fixed; // should a gaussian process be used
int estimate_r; // should the reproduction no be estimated (1 = yes)
int bp_n; // no of breakpoints (0 = no breakpoints)
array[t - seeding_time] int breakpoints; // when do breakpoints occur
int future_fixed; // is underlying future Rt assumed to be fixed
int fixed_from; // Reference date for when Rt estimation should be fixed
int use_pop; // use population size (0 = no; 1 = forecasts; 2 = all)
real<lower = 0> pop_floor; // Minimum susceptible population (numerical stability floor)
int<lower = 0> delay_id_generation_time; // id of generation time
int<lower = 0, upper = 1> growth_method; // method to compute growth rate (0 = infections, 1 = infectiousness)
int backcalc_prior; // Prior type to use for backcalculation
int rt_half_window; // Half the moving average window used when calculating Rt
// prior infections (for backcalculation)
vector<lower = 0>[estimate_r ? 0 : t] shifted_cases;
array[t - seeding_time] int day_of_week; // day of the week indicator (1 - 7)
int model_type; // type of model: 0 = poisson otherwise negative binomial
int week_effect; // length of week effect
int obs_scale; // logical controlling scaling of observations
real obs_weight; // weight given to observation in log density
int likelihood; // Should the likelihood be included in the model
int return_likelihood; // Should the likehood be returned by the model
int<lower = 0> delay_id_truncation; // id of truncation delay
int<lower = 0> delay_id_reporting; // id of reporting delay
int<lower = 0> n_params_variable; // number of parameters
int<lower = 0> n_params_fixed; // number of parameters
vector[n_params_variable] params_lower; // lower bounds of the priors
vector[n_params_variable] params_upper; // upper bounds of the priors

// fixed parameter lookup
array[n_params_fixed + n_params_variable] int<lower = 0> params_fixed_lookup;
// variable parameter lookup
array[n_params_fixed + n_params_variable] int<lower = 0> params_variable_lookup;

vector[n_params_fixed] params_value; // fixed parameter values

// 0 = lognormal; 1 = gamma; 2 = normal
array[n_params_variable] int<lower = 0> prior_dist;
// number of parameters across all parametric delay distributions
int<lower = 0> prior_dist_params_length;
vector[prior_dist_params_length] prior_dist_params;
int<lower = 0> param_id_alpha; // parameter id of alpha (GP magnitude)
int<lower = 0> param_id_rho; // parameter id of rho (GP lengthscale)
int<lower = 0> param_id_R0; // parameter id of R0
int<lower = 0> param_id_fraction_observed; // parameter id of fraction_observed
int<lower = 0> param_id_reporting_overdispersion; // parameter id of reporting_overdispersion
int<lower = 0> param_id_pop; // parameter id of pop

// Priors on the initial value of a centred-GP-wrapped trajectory, applied
// to the derived initial value with a change of variables. Ragged layout
// mirrors prior_dist_params: init_dist_params is a flat vector and the
// per-prior dispatch in estimate_infections.stan advances an offset by the
// appropriate count.
int<lower = 0> n_init_priors;
array[n_init_priors] int<lower = 1> init_param_ids;
array[n_init_priors] int<lower = 0> init_dists;
vector[n_init_priors] init_lower;
vector[n_init_priors] init_upper;
int<lower = 0> init_dist_params_length;
vector[init_dist_params_length] init_dist_params;
}

transformed data {
  // observations
  int ot = t - seeding_time - horizon;  // observed time
  int ot_h = ot + horizon;  // observed time + forecast horizon
  // gaussian process
  int noise_terms = setup_noise(
    ot_h, t, horizon, estimate_r, stationary, future_fixed, fixed_from
  );
  matrix[noise_terms, gp_type == 1 ? 2 * M : M] PHI = setup_gp(
    M, L, noise_terms, gp_type == 1, w0
  );  // basis function

  array[delay_types] int delay_type_max;
  profile("assign max") {
    delay_type_max = get_delay_type_max(
      delay_types, delay_types_p, delay_types_id,
      delay_types_groups, delay_max, delay_np_pmf_groups
    );
  }

  // initial infections scaling (on the log scale)
  real initial_infections_guess;
  if (num_elements(cases) > 0) {
    initial_infections_guess = fmax(
      0,
      log(mean(head(cases, num_elements(cases) > 7 ? 7 : num_elements(cases))))
    );
  } else {
    initial_infections_guess = 0;
  }
}

parameters {
  vector<lower = params_lower, upper = params_upper>[n_params_variable] params;
  // gaussian process
  vector[fixed ? 0 : gp_type == 1 ? 2*M : M] eta;  // unconstrained noise
  // Mean Rt over the observation window for the centred GP parameterisation;
  // the user prior on initial Rt is applied to the derived R[1] in the model
  // block via the init-prior dispatch.
  array[estimate_r] real<lower = 0> R_mean;
  array[estimate_r] real initial_infections;    // seed infections
  // standard deviation of breakpoint effect
  array[bp_n > 0 ? 1 : 0] real<lower = 0> bp_sd;
  vector[bp_n] bp_effects;                   // Rt breakpoint effects
  // delay parameters
  vector<lower = delay_params_lower>[delay_params_length] delay_params;
  // raw gamma values for estimated nonparametric delay PMFs;
  // normalised within each ragged segment to give a Dirichlet draw
  vector<lower = 0>[delay_np_est_length] delay_np_est_raw;
  simplex[week_effect] day_of_week_simplex; // day of week reporting effect
}

transformed parameters {
  // combined fixed + estimated nonparametric delay PMF
  vector[delay_np_pmf_length] delay_np_pmf_use = combine_np_pmf(
    delay_np_pmf, delay_n_np_est, delay_np_est_groups,
    delay_np_est_pos, delay_np_est_raw
  );

  // noise generated by the gaussian process
  vector[fixed ? 0 : noise_terms] noise;
  vector<lower = 0>[estimate_r > 0 ? ot_h : 0] R; // reproduction number
  vector[t] infections; // latent infections
  vector[ot_h] reports; // estimated reported cases
  vector[ot] obs_reports; // observed estimated reported cases
  vector[estimate_r * (delay_type_max[delay_id_generation_time] + 1)]
    gt_rev_pmf;

  // GP in noise - spectral densities
  profile("update gp") {
    if (!fixed) {
      real alpha = get_param(
        param_id_alpha, params_fixed_lookup, params_variable_lookup, params_value,
        params
      );
      real rescaled_rho = 2 * get_param(
        param_id_rho, params_fixed_lookup, params_variable_lookup,
        params_value, params
      ) / noise_terms;
      noise = update_gp(
        PHI, M, L, alpha, rescaled_rho, eta, gp_type, nu
      );
    }
  }

  // Estimate latent infections
  if (estimate_r) {
    profile("gt") {
      gt_rev_pmf = get_delay_rev_pmf(
        delay_id_generation_time, delay_type_max[delay_id_generation_time] + 1,
        delay_types_p, delay_types_id, delay_types_groups, delay_max,
        delay_np_pmf_use, delay_np_pmf_groups, delay_params, delay_params_groups,
        delay_dist, 1, 1, 0
      );
    }
    profile("R0") {
      // R_mean is sampled directly (centred GP scaffolding). The user prior
      // lives on R[1] (initial Rt), applied in the model block via the
      // init_priors plumbing.
      R = update_Rt(
        ot_h, R_mean[1], noise, breakpoints, bp_effects, stationary, ot
      );
    }
    profile("infections") {
      real fraction_observed = get_param(
        param_id_fraction_observed, params_fixed_lookup, params_variable_lookup, params_value,
        params
      );
      real pop = get_param(
        param_id_pop, params_fixed_lookup, params_variable_lookup, params_value,
        params
      );
      infections = generate_infections(
        R, seeding_time, gt_rev_pmf, initial_infections, pop, use_pop, pop_floor,
        future_time, obs_scale, fraction_observed, 1
      );
    }
  } else {
    // via deconvolution
    profile("infections") {
      infections = deconvolve_infections(
        shifted_cases, noise, fixed, backcalc_prior
      );
    }
  }

  // convolve from latent infections to mean of observations
  if (delay_id_reporting) {
    vector[delay_type_max[delay_id_reporting] + 1] reporting_rev_pmf;
    profile("delays") {
      reporting_rev_pmf = get_delay_rev_pmf(
        delay_id_reporting, delay_type_max[delay_id_reporting] + 1,
        delay_types_p, delay_types_id, delay_types_groups, delay_max,
        delay_np_pmf_use, delay_np_pmf_groups, delay_params, delay_params_groups,
        delay_dist, 0, 1, 0
      );
    }
    profile("reports") {
      reports = convolve_to_report(infections, reporting_rev_pmf, seeding_time);
    }
  } else {
    reports = infections[(seeding_time + 1):t];
  }

  // weekly reporting effect
  if (week_effect > 1) {
    profile("day of the week") {
      reports = day_of_week_effect(reports, day_of_week, day_of_week_simplex);
    }
  }

  // scaling of reported cases by fraction observed
  if (obs_scale) {
    profile("scale") {
      real fraction_observed = get_param(
        param_id_fraction_observed, params_fixed_lookup, params_variable_lookup, params_value,
        params
      );
      reports = scale_obs(reports, fraction_observed);
    }
  }

  // truncate near time cases to observed reports
  if (delay_id_truncation) {
    vector[delay_type_max[delay_id_truncation] + 1] trunc_rev_cmf;
    profile("truncation") {
      trunc_rev_cmf = get_delay_rev_pmf(
        delay_id_truncation, delay_type_max[delay_id_truncation] + 1,
        delay_types_p, delay_types_id, delay_types_groups, delay_max,
        delay_np_pmf_use, delay_np_pmf_groups, delay_params, delay_params_groups,
        delay_dist, 0, 1, 1
      );
    }
    profile("truncate") {
      obs_reports = truncate_obs(reports[1:ot], trunc_rev_cmf, 0);
    }
  } else {
    obs_reports = reports[1:ot];
  }

  // accumulate reports
  if (any_accumulate) {
     profile("accumulate") {
       obs_reports = accumulate_reports(obs_reports, accumulate);
    }
  }
}

model {
  // priors for noise GP
  if (!fixed) {
    profile("gp lp") {
      gaussian_process_lp(eta);
    }
  }

  // penalized priors for delay distributions
  profile("delays lp") {
    delays_lp(
      delay_params, delay_params_mean, delay_params_sd, delay_params_groups,
      delay_dist, delay_weight
    );
    delays_np_lp(delay_np_est_raw, delay_np_est_alpha);
  }

  // parameter priors
  profile("param lp") {
    params_lp(
      params, prior_dist, prior_dist_params, params_lower, params_upper
    );
  }

  if (estimate_r) {
    // priors on Rt
    profile("rt lp") {
      rt_lp(
        initial_infections, bp_effects, bp_sd, bp_n,
        cases, initial_infections_guess
      );
    }
  }

  profile("init lp") {
    init_priors_lp(init_param_ids, init_dists, init_dist_params,
                   init_lower, init_upper, param_id_R0, R);
  }

  // observed reports from mean of reports (update likelihood)
  if (likelihood) {
    profile("report lp") {
      real reporting_overdispersion = get_param(
        param_id_reporting_overdispersion, params_fixed_lookup, params_variable_lookup, params_value,
        params
      );
      report_lp(
        cases, case_times, obs_reports, reporting_overdispersion, model_type, obs_weight
      );
    }
  }
}

generated quantities {
  array[it] int imputed_reports;
  vector[estimate_r > 0 ? 0 : ot_h] gen_R;
  vector[ot_h - 1] r;
  vector[return_likelihood ? ot : 0] log_lik;
  // Adjusted Rt accounting for susceptible depletion (only when use_pop > 0)
  vector[(estimate_r > 0 && use_pop > 0) ? ot_h : 0] R_adj;

  profile("generated quantities") {
    real reporting_overdispersion = get_param(
      param_id_reporting_overdispersion, params_fixed_lookup, params_variable_lookup, params_value,
      params
    );
    if (!fixed) {
      real rescaled_rho = 2 * get_param(
        param_id_rho, params_fixed_lookup, params_variable_lookup,
        params_value, params
      ) / noise_terms;
      vector[noise_terms] x = linspaced_vector(noise_terms, 1, noise_terms);
    }

    {
      vector[delay_type_max[delay_id_generation_time] + 1]
        gt_rev_pmf_for_growth;

      if (estimate_r == 0) {
        // sample generation time
        vector[delay_params_length] delay_params_sample = to_vector(normal_lb_rng(
          delay_params_mean, delay_params_sd, delay_params_lower
        ));
        vector[delay_type_max[delay_id_generation_time] + 1] sampled_gt_rev_pmf =
          get_delay_rev_pmf(
            delay_id_generation_time,
            delay_type_max[delay_id_generation_time] + 1, delay_types_p,
            delay_types_id, delay_types_groups, delay_max, delay_np_pmf_use,
            delay_np_pmf_groups, delay_params_sample, delay_params_groups,
            delay_dist, 1, 1, 0
          );
        gt_rev_pmf_for_growth = sampled_gt_rev_pmf;
        // calculate Rt using infections and generation time
        gen_R = calculate_Rt(
          infections, seeding_time, sampled_gt_rev_pmf, rt_half_window
        );
      } else {
        gt_rev_pmf_for_growth = gt_rev_pmf;
      }
  
      // estimate growth from infections
      r = calculate_growth(
        infections, seeding_time, gt_rev_pmf_for_growth, growth_method
        );

      // Calculate adjusted Rt when population adjustment is enabled
      // R_adj = infections / infectiousness (back-calculated from adjusted infections)
      if (estimate_r > 0 && use_pop > 0) {
        R_adj = calculate_Rt(infections, seeding_time, gt_rev_pmf, 0);
      }
    }

    // simulate reported cases
    if (any_accumulate) {
      vector[ot_h] accumulated_reports =
        accumulate_reports(reports, accumulate);
      imputed_reports = report_rng(
        accumulated_reports[imputed_times], reporting_overdispersion, model_type
      );
    } else {
      imputed_reports = report_rng(
        reports[imputed_times], reporting_overdispersion, model_type
      );
    }

    // log likelihood of model
    if (return_likelihood) {
      log_lik = report_log_lik(
        cases, obs_reports[case_times], reporting_overdispersion, model_type, obs_weight
      );
    }
  }
}
