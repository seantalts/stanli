data {
  int<lower=1> J;
  int<lower=1> K;
  matrix[J, K] W;
}
transformed data {
  real s = 0;
  for (k in 1:K) {
    for (j in 1:J) {
      s += W[j, k];
    }
  }
}
parameters {
  real mu;
}
model {
  mu ~ normal(s, 1);
}
