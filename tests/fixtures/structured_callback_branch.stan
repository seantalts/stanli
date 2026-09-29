functions {
  vector rhs(real t, vector y, int N, real rate) {
    real total = 0;
    int trips = t > 0 ? N : N + 1;
    for (i in 1:trips) {
      if (y[1] > 0 && i % 2 == 0)
        total = 0.9 * total + rate * y[1];
      else
        total += rate * y[1] / (N + 1);
    }
    return [-total]';
  }
}
data { int<lower=0> N; }
parameters { real t; vector[1] y; real rate; }
model {
  vector[1] result = rhs(t, y, N, rate);
  target += result[1];
}
