// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: ode_rk45_tol: times has size 0
// CmdStan names the function that the model called, here ode_rk45. The
// kernel always calls the _tol form, so stanli reports ode_rk45_tol.
functions {
  vector rhs(real t, vector y, real k) { return -k * y; }
}
data { array[0] real ts; }
parameters {
  real<lower=0> k;
  vector[2] y0;
}
model {
  array[0] vector[2] z = ode_rk45(rhs, y0, 0.0, ts, k);
  k ~ lognormal(0, 1);
  y0 ~ normal(0, 1);
}
