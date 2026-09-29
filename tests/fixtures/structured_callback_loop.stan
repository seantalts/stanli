functions {
  vector rhs(real t, vector y, int N, real rate) {
    real total = 0;

    for (i in 1:N) total += rate * y[1] / N;
    return [-total]';
  }
}
data { int<lower=0> N; }
parameters { real t; vector[1] y; real rate; }
model {
  vector[1] result = rhs(t, y, N, rate);
  target += result[1];
}
