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
      target += -square(r);
    } else {
      real acc = 0;
      for (j in 1:48) {
        if (a * j < r)
          acc += -0.01 * j * square(r - a);
        else
          acc -= 0.01 * b * j;
      }
      target += -0.5 * square(r) + acc;
    }
  }
}
