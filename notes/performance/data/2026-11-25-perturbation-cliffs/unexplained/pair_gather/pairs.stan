data { int<lower=0> N; int<lower=0> J; array[N] int<lower=1, upper=J> g; vector[N] x; vector[N] y; }
transformed data { matrix[N, 2] Z = append_col(rep_vector(1, N), x); }
parameters { vector[J] a; vector[J] b; real<lower=0> sigma; }
model {
  a ~ normal(0, 1); b ~ normal(0, 1); sigma ~ normal(0, 1);
  matrix[2, J] ab = append_row(a', b');
  vector[N] mu;
  for (n in 1:N) mu[n] = Z[n] * col(ab, g[n]);
  y ~ normal(mu, sigma);
}
