data {
  int<lower=1> T;
  int<lower=2> K;
  array[T, K] real x;
  vector[K] w;
}
parameters {
  real mu;
  real<lower=0> sigma;
}
model {
  array[T, K] real rows;
  vector[T] chain;
  vector[K] ps;
  vector[K] qs = rep_vector(0, K);
  array[T, K] real behind = x;
  array[T, K] real ahead = x;
  vector[K] cs;
  for (r in 1:T) {
    for (j in 1:K) rows[r, j] = mu * x[r, j] + j;
    target += normal_lpdf(rows[r] | 0, sigma);
  }
  chain[1] = mu;
  for (u in 2:T) chain[u] = 0.5 * chain[u - 1] + x[u, 1];
  target += normal_lpdf(chain | 0, 10);
  for (s in 1:T) {
    for (k in 1:K) ps[k] = log(w[k]) + normal_lpdf(x[s, k] | mu * k, sigma);
    target += log_sum_exp(ps);
  }
  for (p in 1:T) {
    for (k in 1:(K - 1)) qs[k] = normal_lpdf(x[p, k] | mu, sigma);
    target += log_sum_exp(qs);
    qs[K] = normal_lpdf(x[p, K] | mu, sigma);
  }
  for (m in 2:T) {
    for (j in 1:K) behind[m, j] = mu * j;
    target += normal_lpdf(behind[m - 1] | 0, sigma);
  }
  for (n in 1:(T - 1)) {
    for (j in 1:K) ahead[n, j] = mu * j;
    ahead[n + 1, 1] = mu;
    target += normal_lpdf(ahead[n] | 0, sigma);
  }
  for (v in 1:T) {
    cs = to_vector(x[v]) * mu;
    for (k in 2:K) cs[k] = cs[k] + cs[k - 1];
    target += normal_lpdf(cs | 0, sigma);
  }
  sigma ~ exponential(1);
}
