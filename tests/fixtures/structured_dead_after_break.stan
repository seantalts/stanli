data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) {
    for (k in 1:1) {
      if (y[n] - mu < 0) target += -square(y[n] - mu);
      else target += -abs(y[n] - mu);
      break;
      target += lgamma(mu + 2);
    }
  }
}
