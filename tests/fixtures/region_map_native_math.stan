data {
  int N;
  vector[N] y;
  vector[N] x;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) {
    real r = y[n] - a * x[n];
    if (r < b) {
      target += -log1p(square(r)) + log(erfc(0.3 * r + b));
    } else {
      target += log1m_exp(-square(r) - 0.1) - inv_square(1.5 + square(b + r));
    }
  }
}
