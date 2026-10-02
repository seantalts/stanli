functions {
  real f(real y, real m1, real m2, real m3, real m4, real m5, real m6) {
    if (y - m1 < 0) return -square(y - m2) - m3;
    return -square(y - m4) * m5 + m6;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real a;
  real b;
}
model {
  vector[N] m1 = a + y * 0.5;
  vector[N] m2 = b * y;
  vector[N] m3 = a * b + y;
  vector[N] m4 = a - y;
  vector[N] m5 = exp(b) + y .* y;
  vector[N] m6 = a + b * y;
  for (n in 1:N) target += f(y[n], m1[n], m2[n], m3[n], m4[n], m5[n], m6[n]);
}
