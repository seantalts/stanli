functions {
  vector runtime_rhs(real t, vector y, real rate) {
    int trips = y[1] > 0 ? 5 : 3;
    real value = 0;
    for (k in 1:trips) {
      if (k == 2) continue;
      value += rate * y[1] / k;
    }
    return [-value]';
  }
}
data { array[2] real times; }
parameters { real rate; }
transformed parameters {
  array[2] vector[1] solution = ode_rk45(runtime_rhs, [1.0]', 0, times, rate);
}
model { rate ~ normal(0, 1); solution[2] ~ normal(0, 1); }
generated quantities {
  array[2] vector[1] other = ode_rk45(runtime_rhs, [-1.0]', 0, times, rate);
}
