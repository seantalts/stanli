data {
  int N;
  matrix[N, N] D;
}
parameters {
  real<lower=0> a;
}
model {
  real s = 0;
  for (i in 1:rows(D))
    for (j in 1:cols(D))
      s += a * square(D[i, j]);
  target += -s;
}
