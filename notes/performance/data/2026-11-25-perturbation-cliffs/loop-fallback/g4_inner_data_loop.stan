data { int N; int J; int D; array[N] int<lower=1, upper=J> idx; vector[N] x; array[D] vector[N] Y; }
parameters { vector[J] alpha; vector[D] beta; vector<lower=0>[D] sigma; }
model {
  for (n in 1:N) {
    for (d in 1:D) {                 // a data-sized inner loop inside the row loop
      Y[d][n] ~ normal(alpha[idx[n]] + beta[d] * x[n], sigma[d]);
    }
  }
}
