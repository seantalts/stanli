// A non-ctsem scan canary for retaining a prepared 55x55 solve across the
// continuation template's replay-and-reverse pair.
//
// The first row is peeled. Data then schedules cheap subject-start rows and a
// stable continuation template. Only the taken parameter arm performs a
// solve; its divisor and dividend both depend on parameters and on carried
// state, so the call cannot be lifted out of the scan. The elementwise matrix
// preparation deliberately makes the structured CFG large enough for the
// production native-adjoint profitability gate without adding another solve.
data {
  int<lower=1> N;
  array[N] int<lower=0, upper=1> new_subject;
  real<lower=1> ridge_scale;
  vector[N] observation;
  real<lower=0> observation_scale;
}
parameters {
  // First so the benchmark can select the conditional with --set-param 0.
  real theta;
  matrix[55, 55] raw;
  vector[55] rhs;
}
model {
  vector[55] state = rhs;
  array[N] real llrow;

  for (i in 0 : (N - 1)) {
    if (i == 0) {
      state = rhs + rep_vector(observation_scale, 55);
    } else if (new_subject[i + 1] == 1) {
      // Subject starts are intentionally cheap and reset the carried state.
      state = rhs + rep_vector(observation_scale, 55);
    } else {
      if (theta > 0) {
        state = mdivide_left(
            0.02 * raw
                + diag_matrix(rep_vector(
                    ridge_scale + observation_scale + 0.001 * state[1], 55))
                + 0.001 * square(raw) + 0.0001 * raw
                + 0.00001 * raw',
            state + theta * rhs);
      } else {
        // The untaken arm keeps the same carry schema without matrix work.
        state = state + theta * rhs + rep_vector(observation_scale, 55);
      }
    }
    llrow[i + 1] = normal_lpdf(observation[i + 1] | state[1], 1);
  }
  target += sum(llrow);
}
