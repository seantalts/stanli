functions {
  vector sho(real t,
             vector y,
             real theta) {
    vector[2] dydt;
    dydt[1] = y[2];
    dydt[2] = -y[1] - theta * y[2];
    return dydt;
  }
}
data {
  int<lower=1> T;
  array[T] vector[2] y;
  real t0;
  array[T] real ts;
}
parameters {
  vector[2] y0;
  vector<lower=0>[2] sigma;
  real theta;
}
model {
  array[T] vector[2] mu = ode_adjoint_tol_ctl(sho, y0, t0, ts, 1e-6/9.0, rep_vector(1e-6/9.0, 2), 1e-6/3.0, rep_vector(1e-6/3.0, 2), 1e-6, 1e-6, 1000000, 150, 1, 2, 2, theta);
  sigma ~ normal(0, 2.5);
  theta ~ std_normal();
  y0 ~ std_normal();
  for (t in 1:T) {
    y[t] ~ normal(mu[t], sigma);
  }
}
