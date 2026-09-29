functions {
  vector rhs(real t, vector y, real rate) { return rep_vector(rate, rows(y)); }
}
transformed data { vector[1] y0 = [0.4]'; array[1] real ts = {0.1}; }
parameters { real rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  real tolerance = 1e-8 + 1e-9 * abs(rate);
  real solved = ode_rk45_tol(rhs, y0, 0, ts, tolerance, tolerance, 100000, rate)[1, 1];
}
