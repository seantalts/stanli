// A recursive function called with a counter that a while loop updates.
functions {
  real rec(int n) {
    if (n <= 0) {
      return -0.75;
    }
    return tanh(rec(n - 1));
  }
}
parameters {
  real mu;
}
model {
  int w = 0;
  while (w < 2) {
    w += 1;
    target += rec(w) * mu;
  }
  mu ~ normal(0, 1);
}
