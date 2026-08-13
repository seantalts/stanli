data {
  int<lower=1, upper=3> N;
}
parameters {
  vector[3] theta;
}
transformed parameters {
  real loop_total = 0;
  for (n in 1 : N) {
    if (theta[n] > 0) {
      loop_total += theta[n];
      continue;
    }
    loop_total -= 2 * theta[n];
  }
  {
    int cursor = 1;
    while (cursor <= N) {
      loop_total += 0.25 * theta[cursor];
      if (cursor == 2) {
        break;
      }
      cursor += 1;
    }
  }
  loop_total += theta[1] > 0 ? theta[2] : theta[3];
}
model {
  target += loop_total;
}
