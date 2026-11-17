data {
  int K;
  array[K] real y;
}
parameters {
  array[3, 2] tuple(real<lower=0>, simplex[4]) grid;
  array[2] tuple(real, array[K] tuple(real<offset=1, multiplier=2>, simplex[3],
                                      cholesky_factor_corr[3])) nested;
  tuple(vector<lower=-1, upper=1>[2], array[2] real) plain;
}
transformed parameters {
  array[2] tuple(real<lower=0>, vector[3]) scaled;
  for (i in 1:2) {
    scaled[i].1 = exp(nested[i].1);
    scaled[i].2 = nested[i].2[1].2 * grid[1, i].1;
  }
}
model {
  for (i in 1:3)
    for (j in 1:2) {
      grid[i, j].1 ~ exponential(1);
      grid[i, j].2 ~ dirichlet(rep_vector(2, 4));
    }
  for (i in 1:2) {
    nested[i].1 ~ normal(0, 1);
    for (k in 1:K) {
      nested[i].2[k].1 ~ normal(y[k], 2);
      nested[i].2[k].2 ~ dirichlet(rep_vector(1.5, 3));
      nested[i].2[k].3 ~ lkj_corr_cholesky(2);
    }
  }
  plain.1 ~ normal(0, 0.5);
  plain.2 ~ normal(0, 1);
  scaled[2].2 ~ normal(0, 3);
}
generated quantities {
  array[3, 2] tuple(real, int) draws;
  for (i in 1:3)
    for (j in 1:2)
      draws[i, j] = (normal_rng(grid[i, j].1, 1), categorical_rng(grid[i, j].2));
  real total = sum(plain.2) + nested[2].2[K].3[3, 3];
}
