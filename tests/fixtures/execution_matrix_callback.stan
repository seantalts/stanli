functions {
  vector matrix_rhs(real t, vector y, matrix coefficients) {
    return -coefficients[2, 3] * y;
  }
}
parameters { matrix[2, 3] coefficients; }
model {
  target += ode_rk45(matrix_rhs, [1.0]', 0, {0.1}, coefficients)[1, 1];
}
