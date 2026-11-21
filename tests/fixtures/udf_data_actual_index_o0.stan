functions {
  real weighted_squares(matrix x, real a) {
    real s = 0;
    for (i in 1:rows(x))
      for (j in 1:cols(x))
        s += a * square(x[i, j]);
    return s;
  }
}
data {
  int N;
  matrix[N, N] D;
}
parameters {
  real<lower=0> a;
}
model {
  target += -weighted_squares(D, a);
}
