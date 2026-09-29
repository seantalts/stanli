functions {
  vector rhs(real t, vector y, real rate) { return rep_vector(rate, rows(y)); }
  vector residual(real t, vector y, vector yp, real rate) { return yp - rhs(t, y, rate); }
  vector system_eq(vector y, real rate) { return y - rep_vector(rate, rows(y)); }
  vector legacy_system(vector y, vector theta, array[] real xr, array[] int xi) {
    return system_eq(y, theta[1]);
  }
  real integrand(real x, real xc, real rate) { return rate * x; }
  real legacy_integrand(real x, real xc, array[] real theta,
                         array[] real xr, array[] int xi) { return theta[1] * x; }
}
transformed data { vector[1] y0 = [0.4]'; array[1] real ts = {0.1}; }
parameters { real rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  real tol = 1e-8 + 1e-9 * abs(rate);
  int steps = rate > 0 ? 1000 : 2000;
  int quadrature_steps = rate > 0 ? 15 : 16;
  int method = rate > 0 ? 1 : 2;
  array[9] real answers;
  if (rate > -100) {
    tol *= 0.5;
    steps += 1;
    answers[1] = dae_tol(residual, y0, [rate]', 0, ts, tol, tol, steps, rate)[1, 1];
    answers[2] = solve_newton_tol(system_eq, y0, 1e-3 + tol, tol, steps, rate)[1];
    answers[3] = solve_powell_tol(system_eq, y0, tol, tol, steps, rate)[1];
    answers[4] = algebra_solver(legacy_system, y0, [rate]',
                                 rep_array(0.0, 0), rep_array(0, 0), tol, tol, steps)[1];
    answers[5] = algebra_solver_newton(legacy_system, y0, [rate]',
                                 rep_array(0.0, 0), rep_array(0, 0), 1e-3 + tol, tol, steps)[1];
    answers[6] = integrate_1d_gauss_kronrod_tol(integrand, 0, 0.2, tol, tol, quadrature_steps, rate);
    answers[7] = integrate_1d_double_exponential_tol(integrand, 0, 0.2, tol, tol, quadrature_steps, rate);
    answers[8] = integrate_1d(legacy_integrand, 0, 0.2, {rate},
                              rep_array(0.0, 0), rep_array(0, 0), tol);
    answers[9] = ode_adjoint_tol_ctl(rhs, y0, 0, ts, tol, rep_vector(tol, 1),
                                    tol, rep_vector(tol, 1), tol, tol, steps,
                                    10 + (rate > 0), 1, method, method, rate)[1, 1];
  }
}
