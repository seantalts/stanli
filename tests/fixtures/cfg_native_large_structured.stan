// A durable non-ctsem cost gate for large structured CFG adjoints.
//
// The parameter-dependent region follows the same necessity-island route as
// the pr236 regression fixture, but its workload is deliberately independent:
// two 55x55 active solves, a matrix exponential, and enough scalar matrix
// preparation to make replaying the entire region a meaningful cost.  The
// fixed for-loop is unrolled by the register-program compiler, leaving a
// forward-only CFG rather than a runtime backedge.
data {
  real<lower=1> ridge_scale;
}
transformed data {
  matrix[55, 55] ridge =
      diag_matrix(rep_vector(ridge_scale, 55));
}
parameters {
  // First on purpose: bench_grad's fixed point gives theta=0.1 and therefore
  // measures the structured arm without a benchmark-only parameter hook.
  real theta;
  matrix[55, 55] raw;
  vector[55] rhs;
  matrix[6, 6] generator;
}
model {
  matrix[55, 55] work = raw + ridge;
  vector[55] state = rhs;
  if (theta > 0) {
    for (iteration in 1:2) {
      work = 0.25 * work + 0.05 * raw + ridge;
      state = mdivide_left(work, state);
      target += sum(state);
    }
    target += sum(matrix_exp(generator));
  } else {
    target += theta;
  }
}
