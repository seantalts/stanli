data {
  int<lower=0> N;
  array[40] int<lower=1, upper=8> gap;
}
parameters {
  real theta;
}
model {
  real state = theta;
  int row = 1;
  real bound = 1;
  while (row <= N) {
    if (row > 1) bound = gap[row - 1];
    real inner = 0;
    while (inner < bound) {
      state = state + inv_logit(state) * 0.01;
      inner += 1;
    }
    row += 1;
  }
  target += state;
}
