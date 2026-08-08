// The modern variadic ODE interface, in one model, so CI has a guard
// where harnesses/ode_sweep.py (which needs a CmdStan checkout) cannot
// run.
//
// The four solvers appear so the dispatch cannot silently collapse to
// one, and the argument list mixes every region of the packing: a scalar
// parameter, a vector parameter, a data real, and a data integer array.
// That last combination is the one that goes wrong quietly -- an
// argument packed into the wrong region still integrates and still
// produces a finite gradient, of the wrong model.
functions {
  vector rhs(real t, vector y, real a, real b) {
    vector[2] dy;
    dy[1] = -a * y[1] + b * y[2];
    dy[2] = a * y[1] - b * y[2];
    return dy;
  }
  vector rhs_mixed(real t, vector y, real a, vector p, real d,
                   array[] int k) {
    vector[2] dy;
    dy[1] = -a * y[1] + p[1] * y[2] + d * k[1];
    dy[2] = a * y[1] - p[2] * y[2];
    return dy;
  }
}
data {
  int<lower=1> N;
  array[N] real ts;
  real d_real;
  array[1] int d_int;
}
parameters {
  real<lower=0> a;
  real<lower=0> b;
  vector<lower=0>[2] p;
  vector<lower=0>[2] y0;
}
transformed parameters {
  array[N] vector[2] z_rk45 = ode_rk45(rhs, y0, 0.0, ts, a, b);
  array[N] vector[2] z_bdf = ode_bdf(rhs, y0, 0.0, ts, a, b);
  array[N] vector[2] z_adams = ode_adams(rhs, y0, 0.0, ts, a, b);
  array[N] vector[2] z_ckrk = ode_ckrk(rhs, y0, 0.0, ts, a, b);
  array[N] vector[2] z_tol =
      ode_rk45_tol(rhs, y0, 0.0, ts, 1e-8, 1e-8, 100000, a, b);
  array[N] vector[2] z_mixed =
      ode_rk45(rhs_mixed, y0, 0.0, ts, a, p, d_real, d_int);
}
model {
  a ~ lognormal(0, 1);
  b ~ lognormal(0, 1);
  p ~ lognormal(0, 1);
  y0 ~ lognormal(0, 1);
  for (n in 1:N) {
    target += sum(z_rk45[n]) + sum(z_bdf[n]) + sum(z_adams[n]);
    target += sum(z_ckrk[n]) + sum(z_tol[n]) + sum(z_mixed[n]);
  }
}
