// Generic scheduled-scan near miss: a row-varying data branch is nested
// beneath an unresolved parameter branch. The scheduler must not freeze that
// data control while selecting a reusable row template.
data {
  int<lower=1> N;
  array[N] int<lower=0, upper=1> new_subject;
  array[N] int<lower=0, upper=1> gate;
  array[N] vector[2] row;
  array[N] real<lower=0> jitter;
}
parameters {
  matrix[2, 2] initial_transition;
  vector[2] initial_state;
  real theta;
  real rho;
}
model {
  matrix[2, 2] transition;
  vector[2] state;
  array[N] real llrow;

  for (i in 0 : (N - 1)) {
    if (i == 0) {
      transition = initial_transition;
      state = initial_state + theta * row[1];
    } else {
      if (new_subject[i + 1] == 1) {
        transition = rho * transition + diag_matrix(row[i + 1]);
        state = transition * initial_state + row[i + 1];
      } else {
        transition = transition + state * row[i + 1]';
        state = transition * state + row[i + 1];
      }

      if (theta > 0) {
        if (gate[i + 1] == 1)
          state = state + exp(theta) * row[i + 1];
        else
          state = state - exp(theta) * row[i + 1];
      } else {
        state = state + square(theta) * row[i + 1];
      }
    }
    llrow[i + 1] = normal_lpdf(row[i + 1] | state, 1 + jitter[i + 1]);
  }
  target += sum(llrow);
}
