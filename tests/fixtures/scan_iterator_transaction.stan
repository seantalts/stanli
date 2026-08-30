data {
  int<lower=1> N;
  int<lower=2> K;
  array[N] int<lower=0, upper=1> restart;
  array[K] int<lower=0, upper=1> gate;
  vector[N] observed;
}
parameters {
  real theta;
}
model {
  real state = 0;
  array[N] real row_score;
  for (row in 0 : (N - 1)) {
    if (row == 0) {
      state = theta;
    } else {
      if (restart[row + 1]) {
        state = theta;
      }
      for (inner in 1:K) {
        if (gate[inner]) {
          if (theta > 0) {
            state += theta * (0.01 * inner);
          } else {
            state += theta * (0.03 * inner);
          }
        } else {
          if (theta > 0) {
            state -= theta * (0.02 * inner);
          } else {
            state -= theta * (0.04 * inner);
          }
        }
      }
    }
    row_score[row + 1] = normal_lpdf(observed[row + 1] | state, 1);
  }
  target += sum(row_score);
}
