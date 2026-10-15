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
      target += -square(r) * inv_logit(a * x[n] + 3 * b)
                - 0.1 * inv_logit(40 * (r + inv_logit(b)));
    } else {
      target += -0.5 * square(r) * inv_logit(b);
    }
  }
}
