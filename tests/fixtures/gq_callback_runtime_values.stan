functions {
  vector rhs(real t, vector y, data real rate, data vector shift,
             data matrix A, data vector empty, array[] int tag) {
    if (rate < 0)
      return rep_vector(2 * rate + shift[1] + A[2, 1] + 0.01 * tag[1] + sum(empty), rows(y));
    return rep_vector(rate + shift[1] + A[2, 1] + 0.01 * tag[1] + sum(empty), rows(y));
  }
  vector residual(real t, vector y, vector yp, data real rate,
                  data vector shift, data matrix A, data vector empty, array[] int tag) {
    return yp - rhs(t, y, rate, shift, A, empty, tag);
  }
  vector system_eq(vector y, data real rate, data vector shift,
                   data matrix A, data vector empty, array[] int tag) {
    return y - rhs(0, y, rate, shift, A, empty, tag);
  }
  real integrand(real x, real xc, data real rate, data vector shift,
                 data matrix A, data vector empty, array[] int tag) {
    return (rate + shift[1] + A[2, 1] + 0.01 * tag[1]) * x;
  }
}
transformed data {
  vector[1] y0 = [0.4]';
  array[1] real ts = {0.1};
  vector[1] atol = [1e-10]';
  array[1] int tag = {3};
}
parameters { real gate; real rate; }
model { gate ~ normal(0, 1); rate ~ normal(0, 1); }
generated quantities {
  vector[1] shift = [0.2 * rate]';
  vector[0] empty = rep_vector(rate, 0);
  matrix[2, 3] A = rep_matrix(0.1 * rate, 2, 3);
  vector[1] yp0 = [(rate < 0 ? 2.3 : 1.3) * rate + 0.03]';
  real graph_ode = ode_rk45(rhs, y0, 0, ts, rate, shift, A, empty, tag)[1, 1];
  real graph_quad = integrate_1d_gauss_kronrod(integrand, 0, 0.2,
                                              rate, shift, A, empty, tag);
  real branch_answer = 0;
  if (gate > 0) {
    branch_answer += ode_rk45(rhs, y0, 0, ts, rate, shift, A, empty, tag)[1, 1];
    branch_answer += dae(residual, y0, yp0, 0, ts, rate, shift, A, empty, tag)[1, 1];
    branch_answer += solve_newton(system_eq, y0, rate, shift, A, empty, tag)[1];
    branch_answer += integrate_1d_gauss_kronrod(integrand, 0, 0.2,
                                               rate, shift, A, empty, tag);
    branch_answer += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                          atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                          rate, shift, A, empty, tag)[1, 1];
  }
}
