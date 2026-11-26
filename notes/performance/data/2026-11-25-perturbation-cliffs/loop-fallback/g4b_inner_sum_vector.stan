data { int N; int K; matrix[N, K] X; vector[N] y; }
parameters { real a; vector[K] b; real<lower=0> s; }
model {
  vector[N] mu;
  for (n in 1:N) {
    mu[n] = a;
    for (k in 1:K) mu[n] += X[n, k] * b[k];   // a sum that stays inside row n
  }
  y ~ normal(mu, s);
}
