data {
  int<lower=1> N;
  vector[3] initial;
  array[2] int<lower=1, upper=3> positions;
}
parameters {
  real theta;
}
model {
  vector[3] state = initial;
  real acc = 0;
  for (row in 1:N) {
    vector[3] before = state;
    state[1] = theta;
    acc += dot_self(state[2:3]);
    acc += sum(state[positions]);
    if (state[2] > 0) acc += theta * state[2];
    state[2] = theta;
    acc += before[2];
    state[positions] = rep_vector(2, 2);
    state[1 + (theta > 0)] = 2 * theta;
    if (state[3] > 0) acc += state[1];
  }
  target += acc + sum(state);
}
