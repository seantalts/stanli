functions {
  vector rhs(real t, vector y, array[] matrix A, array[,] real B, int direction, array[,] int tags) {
    return rep_vector(A[2, 1, 3] + A[1, 2, 1] + B[2, 3] + direction * tags[2, 1], rows(y));
  }
  vector residual(real t, vector y, vector yp, array[] matrix A, array[,] real B, int direction, array[,] int tags) {
    return yp - rhs(t, y, A, B, direction, tags);
  }
  vector system_eq(vector y, array[] matrix A, array[,] real B, int direction, array[,] int tags) {
    return y - rep_vector(A[2, 1, 3] + A[1, 2, 1] + B[2, 3] + direction * tags[2, 1], rows(y));
  }
  real integrand(real x, real xc, array[] matrix A, array[,] real B, int direction, array[,] int tags) {
    return (A[2, 1, 3] + A[1, 2, 1] + B[2, 3] + direction * tags[2, 1]) * x;
  }
}
data { array[2, 3] real B; }
transformed data {
  vector[1] y0 = [0.4]';
  array[1] real ts = {0.1};
  vector[1] atol = [1e-10]';
}
parameters { real gate; real rate; }
model { gate ~ normal(0, 1); rate ~ normal(0, 1); }
generated quantities {
  int direction = rate > 0 ? 2 : 1;
  array[2, 2] int tags = {{rate > 0 ? 1 : 2, 3}, {4, rate < 0 ? 5 : 6}};
  array[2] matrix[2, 3] A = {rep_matrix(rate, 2, 3), rep_matrix(2 * rate, 2, 3)};
  real answer = 0;
  if (gate > -100) {
    direction = rate > 0 ? 3 : 4;
    tags[2, 1] = rate < 0 ? 7 : 8;

    vector[1] yp0 = [3 * rate + B[2, 3] + direction * tags[2, 1]]';


    answer += ode_rk45(rhs, y0, 0, ts, A, B, direction, tags)[1, 1];
    answer += dae(residual, y0, yp0, 0, ts, A, B, direction, tags)[1, 1];
    answer += solve_newton(system_eq, y0, A, B, direction, tags)[1];
    answer += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B, direction, tags);
    answer += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                  atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                  A, B, direction, tags)[1, 1];
  }
}
