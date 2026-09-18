functions {
  real polynomial(array[] real s, int first, int last, vector b,
                  real c, real d, real e, real f, real g, real h,
                  vector alias_b) {
    return sum(s) * (sum(b) + sum(alias_b) + c + d + e + f + g + h);
  }
  real unsafe_index(array[] real s, int first, int last, vector b) {
    return sum(s) * b[b[1] > 0 ? 1 : 2];
  }
}
data {
  int<lower=0> N;
  int<lower=1> grain;
  int<lower=0, upper=1> fixed_partition;
  int<lower=0, upper=1> dynamic_index;
}
parameters {
  array[N] real z;
  vector[8] b;
}
model {
  real r;
  if (dynamic_index) {
    r = reduce_sum(unsafe_index, z, grain, b);
  } else if (fixed_partition) {
    r = reduce_sum_static(polynomial, z, grain, b, b[1], b[2], b[3],
                          b[4], b[5], b[6], b);
  } else {
    r = reduce_sum(polynomial, z, grain, b, b[1], b[2], b[3],
                   b[4], b[5], b[6], b);
  }
  // The reduction is not a target term: its incoming adjoint is nonlinear.
  target += -0.5 * square(r) + sum(z) + sum(b);
}
