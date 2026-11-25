data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
}
model {
  matrix[N, 2] M = append_col(a * x, b + x);
  for (n in 1:N) {
    row_vector[2] r = M[n];
    if (r[1] < r[2])
      target += -square(y[n] - r[1]);
    else
      target += -0.5 * square(y[n] - r[2]) + b;
  }
}
