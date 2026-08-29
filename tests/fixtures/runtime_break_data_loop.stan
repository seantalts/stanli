data {
  int<lower=0> N;
  array[N] int idx;
}
parameters {
  real theta;
}
model {
  for (ri in 1:N) {
    if (idx[ri] > 0) {
      if (theta > 0) break;
    }
    target += theta;
  }
}
