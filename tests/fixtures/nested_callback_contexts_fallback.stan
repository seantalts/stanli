functions {
  vector rhs(real t, vector y, array[] matrix A, array[,] real B, array[,] int tags) {
    array[y[1] > 0 ? 1 : 2] real temporary;
    temporary[1] = A[2, 1, 3];
    return rep_vector(temporary[1] + A[1, 2, 1] + B[2, 3] + tags[2, 1], rows(y));
  }
  vector residual(real t, vector y, vector yp, array[] matrix A, array[,] real B, array[,] int tags) {
    return yp - rhs(t, y, A, B, tags);
  }
  vector system_eq(vector y, array[] matrix A, array[,] real B, array[,] int tags) {
    array[y[1] > 0 ? 1 : 2] real temporary;
    temporary[1] = A[2, 1, 3];
    return y - rep_vector(temporary[1] + A[1, 2, 1] + B[2, 3] + tags[2, 1], rows(y));
  }
  real integrand(real x, real xc, array[] matrix A, array[,] real B, array[,] int tags) {
    array[x > 0 ? 1 : 2] real temporary;
    temporary[1] = A[2, 1, 3];
    return (temporary[1] + A[1, 2, 1] + B[2, 3] + tags[2, 1]) * x;
  }
}
data { array[2, 3] real B; array[2, 2] int tags; }
transformed data {
  vector[1] y0 = [0.4]';
  array[1] real ts = {0.1};
  vector[1] atol = [1e-10]';
}
parameters { real gate; real rate; }
model {
  array[2] matrix[2, 3] A = {rep_matrix(rate, 2, 3), rep_matrix(2 * rate, 2, 3)};

  vector[1] yp0 = [3 * rate + B[2, 3] + tags[2, 1]]';


  target += -0.5 * (square(gate) + square(rate));
  target += ode_rk45(rhs, y0, 0, ts, A, B, tags)[1, 1];
  target += dae(residual, y0, yp0, 0, ts, A, B, tags)[1, 1];
  target += solve_newton(system_eq, y0, A, B, tags)[1];
  target += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B, tags);
  target += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                A, B, tags)[1, 1];
  if (gate > 0) {
    target += ode_rk45(rhs, y0, 0, ts, A, B, tags)[1, 1];
    target += dae(residual, y0, yp0, 0, ts, A, B, tags)[1, 1];
    target += solve_newton(system_eq, y0, A, B, tags)[1];
    target += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B, tags);
    target += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                  atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                  A, B, tags)[1, 1];
  }
}
generated quantities {
  array[2] matrix[2, 3] A = {rep_matrix(rate, 2, 3), rep_matrix(2 * rate, 2, 3)};
  real answer = 0;
  if (gate > 0) {

    vector[1] yp0 = [3 * rate + B[2, 3] + tags[2, 1]]';


    answer += ode_rk45(rhs, y0, 0, ts, A, B, tags)[1, 1];
    answer += dae(residual, y0, yp0, 0, ts, A, B, tags)[1, 1];
    answer += solve_newton(system_eq, y0, A, B, tags)[1];
    answer += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B, tags);
    answer += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                  atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                  A, B, tags)[1, 1];
  }
}
