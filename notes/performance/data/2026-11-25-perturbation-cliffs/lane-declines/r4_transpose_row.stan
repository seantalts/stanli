// Sub-cause 4: a loop-invariant transpose written inside the row loop.
data {
  int<lower=1> N;
  int<lower=1> J;
  array[N] int<lower=1, upper=J> g;
  vector[N] x;
  vector[N] y;
}
transformed data {
  matrix[N, 2] Z = append_col(rep_vector(1, N), x);
}
parameters {
  vector[J] a;
  vector[J] b;
  real<lower=0> sigma;
}
model {
  matrix[2, J] abt = append_row(a', b');
  a ~ normal(0, 1);
  b ~ normal(0, 1);
  for (n in 1:N)
    y[n] ~ normal(Z[n] * (abt')[g[n]]', sigma);
}
