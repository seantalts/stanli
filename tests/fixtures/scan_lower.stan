data {
  int<lower=0> N;
}
parameters {
  real theta;
}
model {
  real state = 0;
  for (i in 1 : N) {
    if (i == 1) {
      state = theta + 1;
    } else {
      state = 0.5 * state + theta;
    }
    target += normal_lpdf(state | theta, 1);
  }
}
