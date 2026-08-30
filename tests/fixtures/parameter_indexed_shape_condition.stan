// Indexing a parameter matrix by data still has data-only geometry.  The
// first condition therefore folds while the second condition, which reads an
// actual selected matrix cell, remains parameter-dependent.
data {
  int<lower=1> N;
  int<lower=1> K;
  array[K] int<lower=1, upper=N> idx;
}
parameters {
  matrix[N, 2] M;
  real theta;
}
model {
  if (rows(M[idx, :]) == K && cols(M[idx, :]) == 2)
    target += 3 * theta;
  else
    target += 30 * theta;

  if (M[idx[1], 1] > 0)
    target += theta;
  else
    target += 2 * theta;
}
