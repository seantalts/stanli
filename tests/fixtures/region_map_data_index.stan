data {
  int N;
  vector[N] y;
  vector[N] x;
  int J;
  int K;
  matrix[N, K] X;
  array[N] int g;
  array[N, 3] int h;
}
parameters {
  real a;
  real ls;
  matrix[K, J] z;
  vector[J] w;
}
model {
  to_vector(z) ~ std_normal();
  w ~ std_normal();
  // The indices are data: X[n], z[, g[n]] and w[h[n]] are proven once.
  for (n in 1:N)
    target += normal_lpdf(y[n] | X[n] * z[, g[n]] + a * x[n], exp(ls))
              + 0.1 * sum(w[h[n]]);
  // The row of z depends on a parameter, so every read is checked.
  for (n in 1:N) {
    int j = (a > 0) + 1;
    target += -square(sum(z[j]) - x[n]);
  }
  // z[, g[n]] runs on every lane; X[n] runs only where a parameter says.
  for (n in 1:N) {
    real m = sum(z[, g[n]]);
    if (a * x[n] > 0)
      target += -square(m - sum(X[n]));
    else
      target += -0.5 * square(m) + ls;
  }
}
