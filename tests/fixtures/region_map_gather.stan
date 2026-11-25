data {
  int N;
  vector[N] y;
  vector[N] x;
  int J;
  int K;
  matrix[N, K] X;
  matrix[K, J] Zd;
  vector[J] bd;
  array[N] int g;
  array[N, 3] int idx;
  array[N] int h;
}
parameters {
  real a;
  real ls;
}
model {
  matrix[K, J] z = a * Zd;
  vector[J] b = ls * bd;
  for (n in 1:N) {
    matrix[2, 2] M = [[a * x[n], ls], [a, ls * x[n]]];
    real m = X[n] * z[, g[n]] + sum(M[h[n]]);
    real r = y[n] - m;
    if (r < ls)
      target += -square(r) + sum(b[idx[n]]);
    else
      target += -0.5 * square(r) + ls;
  }
}
