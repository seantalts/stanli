functions {
  vector pick(real a) {
    if (a < 0) return [1, 2]';
    return [a, a * a]';
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
    vector[2] v = [0, 0]';
    int k = 0;
    while (k < 2) {
      v = v + pick(y[n] - mu - k);
      k += 1;
    }
    target += -square(v[1]) - 0.1 * v[2];
  }
}
