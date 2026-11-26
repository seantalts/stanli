// Sub-cause 6b: a column-major matrix filled one element at a time, row by
// row. With T >= 4 reroll rolls each row's inner loop on its own and leaves
// one row store per row, which no pass fuses again. With T = 3 it fuses all.
data {
  int<lower=1> M;
  int<lower=1> T;
  array[M, T] int<lower=0, upper=1> y;
}
parameters {
  vector[T] a;
  vector[M] e;
}
model {
  matrix[M, T] lp;
  for (i in 1:M)
    for (j in 1:T)
      lp[i, j] = a[j] + e[i];
  a ~ normal(0, 1);
  e ~ normal(0, 1);
  for (i in 1:M) y[i] ~ bernoulli_logit(lp[i]);
}
