data {
  int<lower=0> N;
}
parameters {
  real theta;
}
model {
  int m = 2;
  for (i in 1:N) {
    target += m * theta;
    if (theta > 0) m = 3;
    if (i > N) break;
  }
}
