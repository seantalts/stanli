// Right-hand-side shapes for tests/test_ode_prog.cpp. Not a real model: the
// functions exist to be compiled by compile_rhs and cross-checked against the
// MIR interpreter that compile_rhs replaces.
functions {
  // Straight-line arithmetic, the common case (this is lotka_volterra's).
  array[] real f_lin(real t, array[] real z, array[] real theta,
                     array[] real x_r, array[] int x_i) {
    real u = z[1];
    real v = z[2];
    real du = (theta[1] - theta[2] * v) * u;
    real dv = (-theta[3] + theta[4] * u) * v;
    return {du, dv};
  }
  // A branch on the solve time, a loop over the states, transcendentals, and
  // both data arrays -- x_r arrives per call, x_i is folded at compile time.
  array[] real f_branch(real t, array[] real z, array[] real theta,
                        array[] real x_r, array[] int x_i) {
    array[2] real dz;
    real dose = 0;
    if (t > 0.5) {
      dose = exp(-theta[1] * t) * x_r[1] / x_r[2];
    }
    for (k in 1 : 2) {
      dz[k] = dose - theta[k] * z[k] / (1 + abs(z[k]))
              + x_i[1] * sqrt(square(z[k]) + 1);
    }
    return dz;
  }
  // Calls another function, uses a ternary and a comparison.
  array[] real f_udf(real t, array[] real z, array[] real theta,
                     array[] real x_r, array[] int x_i) {
    return {scale(z[1], theta[1]), z[2] > 0 ? -theta[2] * z[2] : theta[2]};
  }
  real scale(real a, real b) {
    return a * b + inv_logit(a);
  }
  // Returns from inside a branch on a runtime value. Both exits join in the
  // compiled function's result, preserving the selected return expression.
  array[] real f_early(real t, array[] real z, array[] real theta,
                       array[] real x_r, array[] int x_i) {
    if (t > 0.5) {
      return {theta[1] * z[1], theta[2] * z[2]};
    }
    return {-z[1], -z[2]};
  }
  // The loop guard must still select between the early and trailing returns.
  array[] real f_while_early(real t, array[] real z, array[] real theta,
                             array[] real x_r, array[] int x_i) {
    while (t > 0.5) {
      return {theta[1] * z[1], theta[2] * z[2]};
    }
    return {-z[1], -z[2]};
  }

  array[] real f_nested_early(real t, array[] real z, array[] real theta,
                              array[] real x_r, array[] int x_i) {
    if (t > 0.5) {
      if (z[1] > 0) return {theta[1] * z[1], theta[2] * z[2]};
      else return {z[1], z[2]};
    }
    return {-z[1], -z[2]};
  }
  array[] real f_while_pair(real t, array[] real z, array[] real theta,
                            array[] real x_r, array[] int x_i) {
    while (t > 0.5) {
      if (z[1] > 0) return {theta[1] * z[1], theta[2] * z[2]};
      else return {z[1], z[2]};
    }
    return {-z[1], -z[2]};
  }
  real first_scale(real a, real b) {
    for (k in 1:2) return a*b;
    return a;
  }
  real conditional_scale(real a, real b) {
    if (a > 0) return a*b;
    else return a+b;
  }
  array[] real f_call_in_while(real t, array[] real z, array[] real theta,
                               array[] real x_r, array[] int x_i) {
    real value = 0;
    int i = 0;
    while (i < 2) {
      value += first_scale(z[1], theta[1]);
      i += 1;
      if (i == 1) continue;
      value += conditional_scale(z[2], theta[2]);
    }
    return {value, -value};
  }

  array[] real f_loop_exits(real t, array[] real z, array[] real theta,
                            array[] real x_r, array[] int x_i) {
    for (k in 1:3) {
      if (t < 0.3) continue;
      if (t > 0.7) break;
      return {theta[1]*z[1], theta[2]*z[2]};
    }
    return {-z[1], -z[2]};
  }
  array[] real f_return_in_loop(real t, array[] real z, array[] real theta,
                                array[] real x_r, array[] int x_i) {
    real v = z[1];
    int i = 0;
    while (i < 3) {
      i += 1;
      if (i == 1) continue;
      if (t < 0.3) break;
      if (t > 0.7) return {v, z[2]};
      v += theta[1];
    }
    return {v, -z[2]};
  }
  array[] real f_bad_return_shape(real t, array[] real z, array[] real theta,
                                  array[] real x_r, array[] int x_i) {
    if (t > 0.5) return {z[1]};
    return {z[1], z[2]};
  }

}
data {
  int<lower=0> N;
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
}
