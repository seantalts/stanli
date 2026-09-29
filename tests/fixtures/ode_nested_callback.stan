functions {
  vector rhs(real t, vector y, array[] matrix A, array[,] real B,
             array[,] int tags) {
    real slope = A[2, 1, 3] + A[1, 2, 1] + B[2, 3] + tags[2, 1];
    return -slope * y;
  }
}
data {
  array[2, 3] real B;
  array[2, 2] int tags;
}
transformed data { array[2] real times = {0.1, 0.2}; }
parameters { matrix[2, 3] rate; }
transformed parameters {
  array[2] matrix[2, 3] A = {rate, 2 * rate};
  array[2] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, A, B, tags);
}
model {
  to_vector(rate) ~ normal(0, 1);
  solution[2] ~ normal(0, 1);
}
generated quantities {
  array[2] vector[1] other = ode_rk45(rhs, [-1.0]', 0, times, A, B, tags);
}
