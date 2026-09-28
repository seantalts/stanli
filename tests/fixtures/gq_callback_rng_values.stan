functions {
  vector rhs(real t, vector y, data real first, data real second) {
    return rep_vector(first + 2 * second, rows(y));
  }
}
transformed data { vector[1] y0 = [0.4]'; array[1] real ts = {0.1}; }
parameters { real rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  real solved = ode_rk45(rhs, y0, 0, ts, normal_rng(rate, 1),
                         normal_rng(rate, 1))[1, 1];
  real next_draw = normal_rng(rate, 1);
}
