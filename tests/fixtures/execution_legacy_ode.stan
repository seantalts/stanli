functions {
  array[] real graph_rhs(real t, array[] real y, array[] real theta,
                         array[] real x_r, array[] int x_i) {
    if (t > 2) return {theta[1] * y[1]};
    return {-theta[1] * y[1]};
  }
  array[] real region_rhs(real t, array[] real y, array[] real theta,
                          array[] real x_r, array[] int x_i) {
    if (t > 3) return {theta[1] * y[1]};
    return {-theta[1] * y[1]};
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
