functions {
  real f(real y, real mu) {
    if (y - mu < -25)
      return -0.5 * square(y - mu) - log(mu - y) - 0.9189385332046727;
    if (y - mu <= 0) return log_mix(0.1, -1.0, normal_lpdf(y | mu, 1));
    return log(0.5 * erfc(-(y - mu) * 0.7071067811865476));
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
  for (n in 1:N) target += f(y[n], mu) + 0.5 * (y[n] > mu);
}
