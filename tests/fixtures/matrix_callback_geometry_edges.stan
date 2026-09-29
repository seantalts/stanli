functions {
  vector rhs(real t, vector y, matrix D, array[] int tag, matrix A,
             matrix empty_rows, matrix empty_cols) {
    return [A[1, 3] + D[2, 1] + 0.01 * (rows(empty_rows) + cols(empty_rows)),
            A[2, 2] - D[1, 2] + 0.01 * (rows(empty_cols) + cols(empty_cols) + tag[1])]';
  }
  vector dynamic_rhs(real t, vector y, matrix D, array[] int tag, matrix A,
                     matrix empty_rows, matrix empty_cols) {
    array[y[1] > 0 ? 1 : 2] real temporary;
    temporary[1] = t;
    return rhs(temporary[1], y, D, tag, A, empty_rows, empty_cols);
  }
}
data {
  matrix[3, 2] B;
  matrix[0, 3] empty_rows;
  matrix[3, 0] empty_cols;
}
transformed data {
  vector[2] y0 = [0.4, -0.2]';
  array[1] real ts = {0.1};
  array[1] int tag = {3};
}
parameters { real rate; }
transformed parameters {
  matrix[3, 2] A = rep_matrix(rate, 3, 2);
  A[2, 1] = 2 * rate;
  A[3, 1] = 3 * rate;
  A[2, 2] = 4 * rate;
  array[1] vector[2] answer = ode_rk45(rhs, y0, 0, ts, B', tag,
                                     A' * diag_matrix(rep_vector(1, 3)), empty_rows, empty_cols);
  array[1] vector[2] fallback_answer = ode_rk45(dynamic_rhs, y0, 0, ts, B', tag,
                                              A' * diag_matrix(rep_vector(1, 3)), empty_rows, empty_cols);
}
model {
  rate ~ normal(0, 1);
  answer[1] ~ normal(0, 1);
  fallback_answer[1] ~ normal(0, 1);
}
generated quantities {
  real value_answer = 0;
  if (rate > 0) {
    value_answer = sum(ode_rk45(rhs, y0, 0, ts, B', tag,
                               A', empty_rows, empty_cols)[1]);
  }
}
