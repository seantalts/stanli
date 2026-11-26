// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 4, "J": [1, 2, 3, 4], "Z": [1, 1, 1, 1], "y": [0.1, 0.4, -0.3, 1.2]}
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK-NOT: GATHER
// STANLI-LIT-CHECK: s{{[0-9]+}}[4] = MUL s{{[0-9]+}}[4,P] s{{[0-9]+}}[1]
// STANLI-LIT-CHECK-NOT: LOOP
// brms writes an observation-level effect this way. J names every element
// of r in order, so r[J] is r and there is no GATHER. Z is all ones, so the
// product reads one scalar where the vector statement names Z[1:N].
data {
  int<lower=1> N;
  array[N] int<lower=1, upper=N> J;
  vector[N] Z;
  vector[N] y;
}
parameters {
  real b;
  vector[N] r;
  real<lower=0> sigma;
}
model {
  vector[N] mu = rep_vector(0.0, N);
  mu += b;
  for (n in 1 : N) {
    mu[n] += r[J[n]] * Z[n];
  }
  target += normal_lpdf(y | mu, sigma);
}
