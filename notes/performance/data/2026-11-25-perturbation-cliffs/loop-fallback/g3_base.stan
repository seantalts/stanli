data { int N; int J; array[N] int<lower=1, upper=J> idx; vector[N] x; vector[N] y; }
parameters { vector[J] alpha; real beta; real<lower=0> sigma; }
model {
  vector[N] mu;
  for (n in 1:N) {
    mu[n] = alpha[idx[n]] + beta * x[n];
    target += normal_lpdf(y[n] | mu[n], sigma);
  }
}
