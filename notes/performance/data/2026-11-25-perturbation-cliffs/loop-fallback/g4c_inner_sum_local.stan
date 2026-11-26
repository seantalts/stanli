data { int N; int K; matrix[N, K] X; vector[N] y; }
parameters { real a; vector[K] b; real<lower=0> s; }
model {
  for (n in 1:N) {
    real acc = a;
    for (k in 1:K) acc += X[n, k] * b[k];     // acc is declared in the row body
    target += normal_lpdf(y[n] | acc, s);
  }
}
