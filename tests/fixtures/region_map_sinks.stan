data {
  int N;
}
parameters {
  real a;
  real b;
}
model {
  for (n in 1:N) {
    real t = 0.05 * n - 1.0;
    real k = 1.5;
    real flag = 0;
    if (t > 0.2) {
      k = 2.5;
      flag = 1;
    }
    real w = 0.5;
    if (t > 0.3) w = 0.25;
    real r = t - k * a;
    if (flag > 0.5)
      target += -w * square(r) - b * b;
    else
      target += -0.5 * square(r) + b;
  }
}
