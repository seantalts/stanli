functions {
  real structured_value(real theta, matrix x) {
    int done = 0;
    real out = 0;
    while (theta > done) {
      matrix[2, 2] a = add_diag(crossprod(x), 2.0);
      matrix[2, 2] row_diag = add_diag(crossprod(x), [2.0, 3.0]);
      matrix[2, 2] e = matrix_exp(-a);
      matrix[2, 2] solved = a \ x;
      matrix[2, 2] right_spd = mdivide_right_spd(x, a);
      matrix[2, 2] q = quad_form_sym(a, x);
      matrix[2, 2] tc = tcrossprod(x);
      out += e[1, 1] - 0.7 * e[2, 1] + 1.3 * solved[1, 2]
             + 0.6 * right_spd[2, 1] + 0.4 * q[2, 2]
             - 0.2 * tc[1, 2] + 0.05 * row_diag[2, 2];
      done += 1;
    }
    return out;
  }
}
parameters {
  real theta;
  matrix[2, 2] x;
}
model {
  target += theta + structured_value(theta, x);
}
generated quantities {
  real score = structured_value(theta, x);
}
