functions {
  vector rhs(real t, vector y, real rate) {
    return rep_vector(rate, rows(y));
  }
  array[] real legacy_rhs(real t, array[] real y, array[] real theta,
                          array[] real xr, array[] int xi) {
    return {theta[1]};
  }
}
transformed data { vector[1] y0 = [0.4]'; array[1] real ts = {0.1}; }
parameters { real rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  real tol = 1e-8 + 1e-9 * abs(rate);
  int steps = rate > 0 ? 10000 : 20000;
  array[7] real answers;
  if (rate > -100) {
    tol = tol * 0.5;
    steps = steps + 1;
    answers[1] = ode_rk45_tol(rhs, y0, 0, ts, tol, tol, steps, rate)[1, 1];
    answers[2] = ode_bdf_tol(rhs, y0, 0, ts, tol, tol, steps, rate)[1, 1];
    answers[3] = ode_adams_tol(rhs, y0, 0, ts, tol, tol, steps, rate)[1, 1];
    answers[4] = ode_ckrk_tol(rhs, y0, 0, ts, tol, tol, steps, rate)[1, 1];
    answers[5] = integrate_ode_rk45(legacy_rhs, {0.4}, 0, ts, {rate},
                                   rep_array(0.0, 0), rep_array(0, 0), tol, tol, steps)[1, 1];
    answers[6] = integrate_ode_bdf(legacy_rhs, {0.4}, 0, ts, {rate},
                                  rep_array(0.0, 0), rep_array(0, 0), tol, tol, steps)[1, 1];
    answers[7] = integrate_ode_adams(legacy_rhs, {0.4}, 0, ts, {rate},
                                    rep_array(0.0, 0), rep_array(0, 0), tol, tol, steps)[1, 1];
  }
}
