functions {
  real partial(array[] real slice, int start, int end,
               tuple(real, vector) shared, array[] tuple(int, vector) obs,
               real sigma) {
    real lp = 0;
    for (n in start:end)
      lp += normal_lpdf(slice[n - start + 1] | shared.1 + obs[n].1
                        + dot_product(shared.2, obs[n].2), sigma);
    return lp;
  }

  tuple(real, real) moments(vector x) {
    return (mean(x), sd(x));
  }

  tuple(real, real) moments(array[] real x) {
    return (mean(x), sd(x));
  }

  real partial_moments(array[] real slice, int start, int end, vector beta) {
    tuple(real, real) m = moments(beta);
    return normal_lpdf(slice | m.1, m.2 + 1);
  }
}
data {
  int N;
  array[N] real y;
  array[N] tuple(int, vector[2]) obs;
}
parameters {
  real alpha;
  vector[2] beta;
  real<lower=0> sigma;
}
model {
  alpha ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma ~ exponential(1);
  target += reduce_sum(partial, y, 2, (alpha, beta), obs, sigma);
  target += reduce_sum_static(partial_moments, y, 1, beta);
}
generated quantities {
  real lp_obs = reduce_sum(partial, y, 3, (alpha, beta), obs, sigma);
}
