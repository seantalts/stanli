functions {
  vector rhs(real t, vector y, real rate) {
    int count = y[1] > 0 ? 1 : -1;
    for (k in 1:4) {
      count = count * 2;
      count = count + 3;
      count = count - 1;
    }
    count = count / 3;
    count = count % 7;
    count = abs(-count);
    count = sum({count, 1});
    return [-rate * y[1] * count]';
  }
}
data { array[2] real times; }
parameters { real rate; }
transformed parameters {
  array[2] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, rate);
}
model {
  rate ~ normal(0, 1);
  solution[2] ~ normal(0, 1);
}
generated quantities {
  array[2] vector[1] other = ode_rk45(rhs, [-1.0]', 0, times, rate);
}
