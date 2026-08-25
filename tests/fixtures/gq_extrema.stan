data {
  int<lower=0> N;
  vector[N] d;
}
parameters {
  vector[N] x;
  row_vector[N] xr;
}
model {
  x ~ normal(0, 1);
  xr ~ normal(0, 1);
}
generated quantities {
  real x_min = min(x);
  real x_max = max(x);
  real xr_min = min(xr);
  real xr_max = max(xr);
  real d_min = min(d);
  real d_max = max(d);
}
