functions {
  real partial(array[] real y, int first, int last, real mu) {
    return normal_lpdf(y | mu, 1);
  }
}
data { int N; array[N] real y; }
parameters { real mu; }
model { mu ~ normal(0, 1); target += reduce_sum(partial, y, 1000, mu); }
generated quantities { real draw = normal_rng(mu, 1); }
