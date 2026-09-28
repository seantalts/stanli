functions {
  vector flat_rhs(real t, vector y, vector A, vector B) {
    vector[2] result;
    for (i in 1:2) {
      result[i] = A[i + 0] * y[1] + A[i + 2] * y[2]
                  + A[i + 4] + B[i + 0] * t + B[i + 2] + B[i + 4];
    }
    return result;
  }
}
data {
  vector[2] initial;
  matrix[2, 3] B;
  array[2] real times;
}
parameters {
  real log_rate;
}
transformed parameters {
  matrix[2, 3] A = rep_matrix(0, 2, 3);
  A[1, 1] = -exp(log_rate);
  A[1, 2] = 0.1;
  A[1, 3] = 0.2;
  A[2, 1] = 0.05;
  A[2, 2] = -0.5 * exp(log_rate);
  A[2, 3] = -0.1;
  array[2] vector[2] solution = ode_rk45(flat_rhs, initial, 0, times, to_vector(A), to_vector(B));
}
model {
  log_rate ~ normal(0, 1);
  solution[2] ~ normal(0, 1);
}
