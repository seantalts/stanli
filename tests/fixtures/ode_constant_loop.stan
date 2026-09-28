functions {
  vector rhs(real t, vector y, int N, real rate) {
    real total = 0;

    for (i in 1:N) total += rate * y[1] / N;
    return [-total]';
  }
}
data { int<lower=1> N; }
transformed data { array[1] real times = {0.1}; }
parameters { real<lower=0, upper=1> rate; }
transformed parameters {
  array[1] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, N, rate);
}
model { rate ~ normal(0, 1); solution[1] ~ normal(0, 1); }
