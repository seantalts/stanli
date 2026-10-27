data {
  int N;
  vector[N] z;
}
parameters {
  real mu;
}
model {
  matrix[2, 3] m = rep_matrix(0, 2, 3);
  for (n in 1:N) {
    if (mu > 100) target += -1;
    m[1, 1] = z[n];
    target += normal_lpdf(to_vector(mu * m) | 0, 1);
    target += normal_lpdf(m[1] | mu, 1);
    m[2, 3] = z[n] * 2;
  }
}
