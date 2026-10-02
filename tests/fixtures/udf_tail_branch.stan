functions {
  real log_Phi_tail(real x) {
    if (x < -25) return -0.5 * square(x) - log(-x) - 0.9189385332046727;
    return log(0.5 * erfc(-x * 0.7071067811865476));
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
  for (n in 1:N) target += log_Phi_tail(y[n] - mu);
}
