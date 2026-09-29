functions {
  vector rhs(real t, vector y, matrix rates) {
    array[2, 3] int counts = {{1, 2, 3}, {4, 5, 6}};
    matrix[2, 3] work = rates;
    int n = y[1] > 0 ? 2 : 3;
    for (k in 1:n) {
      int i = k % 2 + 1;
      int j = k % 3 + 1;
      counts[i, j] += k;
      work[i, j] += 0.01 * y[1];
    }
    int row = y[1] > 0 ? 1 : 2;
    int col = y[1] > 0 ? 2 : 3;
    return rep_vector(-abs(work[row, col]) * (1 + 0.01 * counts[row, col]) * y[1], rows(y));
  }
}
transformed data { array[2] real times = {0.1, 0.2}; }
parameters { real log_rate; }
transformed parameters {
  matrix[2, 3] rates = rep_matrix(exp(log_rate), 2, 3);
  array[2] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, rates);
}
model { log_rate ~ normal(0, 1); solution[2] ~ normal(0, 1); }
generated quantities {
  array[2] vector[1] other = ode_rk45(rhs, [-1.0]', 0, times, rates);
}
