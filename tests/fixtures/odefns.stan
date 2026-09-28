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
  // Fixed-size output, but bounds and control change between invocations.
  array[] real f_runtime_for(real t, array[] real z, array[] real theta,
                             array[] real x_r, array[] int x_i) {
    int lo = t > 0.5 ? -2 : 2;
    int hi = z[1] > 0 ? 4 : 1;
    real value = z[1];
    real hits = 0;
    for (k in lo:hi) {
      lo = 7; // The lower bound is evaluated only at loop entry.
      if (k == 0) continue;
      if (k == 3 && t > 0.7) break;
      hits += 1;
      value += theta[1] * k;
    }
    return {value, hits * z[2]};
  }
  array[] real f_runtime_for_nested(real t, array[] real z, array[] real theta,
                                    array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 3 : 0;
    real value = 0;
    for (i in 1:trips) {
      int flag = 0;
      if (i > 1) flag = 1;
      real hits = 0;
      for (j in 1:i) {
        hits += 1;
        if (j == 2) continue;
        value += first_scale(z[1], theta[1]) * hits;
      }
      real k = 0;
      while (k < 2) {
        k += 1;
        value += z[2];
      }
      value += hits + flag;
    }
    return {value, -value};
  }
  array[] real f_runtime_for_return(real t, array[] real z, array[] real theta,
                                    array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 3 : 0;
    for (i in 1:trips) {
      if (i == 1) continue;
      if (z[1] > 0) return {theta[1] * z[1], z[2]};
      break;
    }
    return {z[1], -z[2]};
  }
  array[] real f_runtime_for_max(real t, array[] real z, array[] real theta,
                                 array[] real x_r, array[] int x_i) {
    int first = t > 0.5 ? 2147483647 : 2147483646;
    real value = z[1];
    for (i in first:2147483647) value += theta[1];
    return {value, z[2]};
  }
  array[] real f_runtime_for_shape(real t, array[] real z, array[] real theta,
                                   array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 3 : 0;
    real value = z[1];
    for (i in 1:trips) {
      vector[i] scratch = rep_vector(z[1], i);
      value += sum(scratch);
    }
    return {value, z[2]};
  }
  array[] real f_runtime_for_in_while(real t, array[] real z, array[] real theta,
                                      array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 3 : 0;
    real outer = 0;
    real value = 0;
    while (outer < 2) {
      int hits = 0;
      for (i in 1:trips) hits = i;
      value += hits * z[1];
      outer += 1;
    }
    return {value, z[2]};
  }

  array[] real f_runtime_for_shift(real t, array[] real z,
                                   array[] real theta,
                                   array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 4 : 0;
    real previous = 0;
    real value = z[1];
    for (k in 1:trips) {
      real current = theta[1] * k;
      value += previous;
      previous = current;
    }
    return {value, previous * z[2]};
  }
  array[] real f_runtime_for_int_data(real t, array[] real z,
                                      array[] real theta,
                                      array[] real x_r, array[] int x_i) {
    int first = t > 0.5 ? 1 : 2;
    real value = 0;
    for (k in first:size(x_i)) value += x_i[k] * z[1];
    return {value, z[2]};
  }
  array[] real f_no_loop_integer_overflow(real t, array[] real z,
                                          array[] real theta,
                                          array[] real x_r, array[] int x_i) {
    int count = t > 0.5 ? 2147483647 : 1;
    count = count + 1;
    return {count * z[1], z[2]};
  }

  array[] real f_runtime_for_mutates_bound(real t, array[] real z,
                                           array[] real theta,
                                           array[] real x_r, array[] int x_i) {
    int end = t > 0.5 ? 3 : 0;
    real value = 0;
    for (i in 1:end) {
      value += z[1];
      end = 1;
    }
    return {value, z[2]};
  }

  array[] real f_runtime_for_int_array(real t, array[] real z,
                                       array[] real theta,
                                       array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 2 : 1;
    array[1] int pos = {1};
    for (i in 1:trips) pos[1] = i;
    return {z[pos[1]], z[2]};
  }

  array[] real f_runtime_for_integer_overflow(real t, array[] real z,
                                               array[] real theta,
                                               array[] real x_r, array[] int x_i) {
    int trips = t > 0.5 ? 2 : 1;
    int count = 2147483647;
    for (i in 1:trips) count += 1;
    return {count * z[1], z[2]};
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
