functions {
  vector rhs(real t, vector y, matrix A, matrix B) {
    return rep_vector(A[1, 3] + B[2, 1], rows(y));
  }
  vector residual(real t, vector y, vector yp, matrix A, matrix B) {
    return yp - rhs(t, y, A, B);
  }
  vector system_eq(vector y, matrix A, matrix B) {
    return y - rep_vector(A[1, 3] + B[2, 1], rows(y));
  }
  real integrand(real x, real xc, matrix A, matrix B) {
    return (A[1, 3] + B[2, 1]) * x;
  }
}
data { matrix[2, 3] B; }
transformed data {
  vector[1] y0 = [0.4]';
  array[1] real ts = {0.1};
  vector[1] atol = [1e-10]';
}
parameters { real gate; real rate; }
model {
  matrix[2, 3] A = rep_matrix(rate, 2, 3);
  
  vector[1] yp0 = [rate + B[2, 1]]';
  
  
  target += -0.5 * (square(gate) + square(rate));
  target += ode_rk45(rhs, y0, 0, ts, A, B)[1, 1];
  target += dae(residual, y0, yp0, 0, ts, A, B)[1, 1];
  target += solve_newton(system_eq, y0, A, B)[1];
  target += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B);
  target += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                A, B)[1, 1];
  if (gate > 0) {
    target += ode_rk45(rhs, y0, 0, ts, A, B)[1, 1];
    target += dae(residual, y0, yp0, 0, ts, A, B)[1, 1];
    target += solve_newton(system_eq, y0, A, B)[1];
    target += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B);
    target += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                  atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                  A, B)[1, 1];
  }
}
generated quantities {
  matrix[2, 3] A = rep_matrix(rate, 2, 3);
  real answer = 0;
  if (gate > 0) {
    
    vector[1] yp0 = [rate + B[2, 1]]';
    
    
    answer += ode_rk45(rhs, y0, 0, ts, A, B)[1, 1];
    answer += dae(residual, y0, yp0, 0, ts, A, B)[1, 1];
    answer += solve_newton(system_eq, y0, A, B)[1];
    answer += integrate_1d_gauss_kronrod(integrand, 0, 0.2, A, B);
    answer += ode_adjoint_tol_ctl(rhs, y0, 0, ts, 1e-10, atol, 1e-10,
                                  atol, 1e-10, 1e-10, 100000, 10, 1, 1, 1,
                                  A, B)[1, 1];
  }
}
