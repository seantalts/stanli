// Generic coverage for the opening-gate adapter which canonicalizes a
// scheduled recurrence before OP_SCAN lowering.  The likelihood sink starts
// at exact zeros, then receives two same-cell read-modify-write updates.
data {
  int<lower=2> N;
  array[N] int<lower=0, upper=1> active;
  array[N] int<lower=0, upper=1> restart;
  vector[N] clock;
  vector[N] input;
  vector[N] observed;
}
parameters {
  real theta;
  real initial_state;
  real<lower=0> sigma;
}
model {
  real state = initial_state;
  array[N] real score;
  for (cell in 1 : N)
    score[cell] = 0;
  int previous = 0;

  for (iteration in 0 : N) {
    int current;
    current = iteration ? iteration : 1;
    if (iteration == 0 || active[current]) {
      if (iteration == 0)
        state = initial_state;
      else if (restart[current])
        state = initial_state + theta * input[current];
      else
        state = 0.8 * state
                + theta * input[current]
                + clock[current] - clock[previous];
      if (iteration > 0) {
        score[current] += normal_lpdf(observed[current] | state, sigma);
        score[current] += normal_lpdf(0 | state, 2);
      }
    }
    previous = iteration;
  }
  target += sum(score);
}
