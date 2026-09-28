functions {
  vector rhs(real t, vector y, real rate, array[] int tags) {
    return rep_vector(rate * tags[1], rows(y));
  }
}
transformed data { vector[1] y0 = [0.4]'; array[1] real ts = {0.1}; }
parameters { real rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  array[1] int tags = {rate > 0 ? 1 : 2};
  real solved = ode_rk45(rhs, y0, 0, ts, rate, tags)[1, 1];
}
