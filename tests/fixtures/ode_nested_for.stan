functions {
  int add_hits(int initial, int trips) {
    int result = initial;
    for (j in 1:trips) result += j;
    return result;
  }
  vector rhs(real t, vector y, int N, real rate) {
    int outer = 0;
    real total = 0;
    while (outer < 3) {
      int hits = 1;
      int trips = t > 0 ? N : N + 1;
      if (outer == 1) trips = 0;
      for (i in 1:trips) {
        if (i % 2 == 0) continue;
        hits += 1;
        total += rate * y[1] / (N + 1);
        if (hits > 3) break;
      }
      hits += add_hits(1, trips);
      total += hits * rate * y[1] / 10;
      outer += 1;
    }
    return [-total]';
  }
}
data { int<lower=1, upper=32> N; }
transformed data { array[2] real times = {0.05, 0.1}; }
parameters { real<lower=0, upper=1> rate; }
transformed parameters {
  array[2] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, N, rate);
}
model { rate ~ normal(0, 1); solution[2] ~ normal(0, 1); }
