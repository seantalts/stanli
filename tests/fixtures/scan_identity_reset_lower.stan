// Scheduled-scan carry acceptance: subject-start rows replace the active
// carry without reading its predecessor, while continuation rows leave that
// carry literally unchanged.  The row sink observes both behaviors.
data {
  int<lower=1> N;
  array[N] int<lower=0, upper=1> new_subject;
  array[N] real row;
}
parameters {
  real initial_state;
  real theta;
}
model {
  real state;
  array[N] real llrow;

  for (i in 0 : (N - 1)) {
    if (i == 0) {
      state = initial_state;
    } else {
      if (new_subject[i + 1] == 1)
        state = theta + row[i + 1];
    }
    llrow[i + 1] = normal_lpdf(row[i + 1] | state, 1);
  }
  target += sum(llrow);
}
