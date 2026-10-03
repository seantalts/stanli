functions {
  vector tails(real a) {
    if (a < 0) return [a, 1]';
    return [1, a]';
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) {
    vector[2] t = tails(y[n] - mu);
    target += -square(t[1]) - 0.5 * square(t[2]);
  }
}
