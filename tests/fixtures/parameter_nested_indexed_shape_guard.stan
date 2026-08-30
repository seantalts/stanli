data {
  int<lower=1> K;
  array[K] int<lower=1, upper=3> idx;
}
parameters {
  matrix[3, 2] M;
  real theta;
}
model {
  if (theta > 0) {
    if (rows(M[idx, :]) == K)
      target += theta;
    else
      target += 2 * theta;
  } else {
    target += 3 * theta;
  }
}
