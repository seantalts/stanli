// Fixed-scan dependency fixture. The active carry begins with data but becomes
// parameter-dependent in the stable suffix. The second carry and gain remain
// data-only, even though both participate in every row's arithmetic.
data {
  int<lower=0> N;
  real gain;
  real initial_data_state;
}
parameters {
  real theta;
}
model {
  real active_state = 0;
  real data_state = initial_data_state + gain;
  for (i in 1 : N) {
    if (i == 1) {
      active_state = theta + gain;
      data_state = initial_data_state + gain;
    } else {
      active_state = gain * active_state + theta;
      data_state = 0.5 * data_state + gain;
    }
    target += normal_lpdf(active_state | theta, 1);
  }
}
