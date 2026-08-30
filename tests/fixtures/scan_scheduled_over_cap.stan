// A generic scheduled recurrence with four independent row controls. The
// sixteen reachable control paths intentionally exceed the scheduler's
// reusable-template cap, so lowering must fall back to ordinary graph work.
data {
  int<lower=2> N;
  array[N] int<lower=0, upper=1> gate_a;
  array[N] int<lower=0, upper=1> gate_b;
  array[N] int<lower=0, upper=1> gate_c;
  array[N] int<lower=0, upper=1> gate_d;
  vector[N] observation;
}
parameters {
  real theta;
}
model {
  real state = theta;
  array[N] real row_score;

  for (i in 0 : (N - 1)) {
    if (i == 0) {
      state = theta + observation[1];
    } else {
      if (gate_a[i + 1]) {
        if (gate_b[i + 1]) {
          if (gate_c[i + 1]) {
            if (gate_d[i + 1])
              state += theta * observation[i + 1];
            else
              state -= theta * observation[i + 1];
          } else {
            if (gate_d[i + 1])
              state += 2 * theta * observation[i + 1];
            else
              state -= 2 * theta * observation[i + 1];
          }
        } else {
          if (gate_c[i + 1]) {
            if (gate_d[i + 1])
              state += 3 * theta * observation[i + 1];
            else
              state -= 3 * theta * observation[i + 1];
          } else {
            if (gate_d[i + 1])
              state += 4 * theta * observation[i + 1];
            else
              state -= 4 * theta * observation[i + 1];
          }
        }
      } else {
        if (gate_b[i + 1]) {
          if (gate_c[i + 1]) {
            if (gate_d[i + 1])
              state += 5 * theta * observation[i + 1];
            else
              state -= 5 * theta * observation[i + 1];
          } else {
            if (gate_d[i + 1])
              state += 6 * theta * observation[i + 1];
            else
              state -= 6 * theta * observation[i + 1];
          }
        } else {
          if (gate_c[i + 1]) {
            if (gate_d[i + 1])
              state += 7 * theta * observation[i + 1];
            else
              state -= 7 * theta * observation[i + 1];
          } else {
            if (gate_d[i + 1])
              state += 8 * theta * observation[i + 1];
            else
              state -= 8 * theta * observation[i + 1];
          }
        }
      }
    }
    row_score[i + 1] = normal_lpdf(observation[i + 1] | state, 1);
  }
  target += sum(row_score);
}
