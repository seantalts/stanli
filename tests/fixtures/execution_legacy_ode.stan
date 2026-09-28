functions {
  array[] real graph_rhs(real t, array[] real y, array[] real theta,
                         array[] real x_r, array[] int x_i) {
    int n = t > 2 ? 2 : 1;
    array[n] real unused = rep_array(0.0, n);
    if (t > 2) return {theta[1] * y[1]};
    return {-theta[1] * y[1] + unused[n]};
  }
  array[] real region_rhs(real t, array[] real y, array[] real theta,
                          array[] real x_r, array[] int x_i) {
    int n = t > 3 ? 2 : 1;
    array[n] real unused = rep_array(0.0, n);
    if (t > 3) return {theta[1] * y[1]};
    return {-theta[1] * y[1] + unused[n]};
  }
}
parameters { real rate; }
model {
  target += integrate_ode_rk45(graph_rhs, {1.0}, 0, {0.1}, {rate},
                              rep_array(0.0, 0), rep_array(0, 0))[1, 1];
  if (rate > 0) {
    target += integrate_ode_rk45(region_rhs, {1.0}, 0, {0.1}, {rate},
                                rep_array(0.0, 0), rep_array(0, 0))[1, 1];
  }
}
