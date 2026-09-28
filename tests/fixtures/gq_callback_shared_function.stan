functions {
  vector rhs(real t, vector y, real rate) {
    return rep_vector(rate, rows(y));
  }
  vector system_eq(vector y, vector theta, array[] real xr, array[] int xi) {
    return y - rep_vector(theta[1] + sum(xr) + sum(xi), rows(y));
  }
  real solve_once(real rate) {
    vector[1] y0 = [0.4]';
    array[1] real ts = {0.1};
    real value = ode_rk45(rhs, y0, 0, ts, rate)[1, 1];
    if (rate > 0)
      value += algebra_solver(system_eq, y0, [rate]', rep_array(0.0, 0), rep_array(0, 0))[1];
    return value;
  }
}
parameters { real rate; }
model { rate ~ normal(0, 1); target += solve_once(rate); }
generated quantities { real solved = solve_once(rate); }
