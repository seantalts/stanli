// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL integrate_ode_rk45: times is not a valid sorted vector. The element at 2 is 1, but should be greater than or equal to the previous element, 2
functions {
  array[] real sho(real t, array[] real y, array[] real theta,
                   array[] real x, array[] int x_int) {
    array[2] real dydt;
    dydt[1] = y[2];
    dydt[2] = -y[1] - theta[1] * y[2];
    return dydt;
  }
}
transformed data {
  array[0] real x;
  array[0] int x_int;
  array[2] real ts = {2, 1};
  array[2, 2] real y_hat = integrate_ode_rk45(sho, {1.0, 0.0}, 0.0, ts, {0.5}, x, x_int, 1e-10, 1e-10, 1e6);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
