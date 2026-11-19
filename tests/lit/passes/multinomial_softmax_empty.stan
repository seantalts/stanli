// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: Probabilities parameter has size 0
// STANLI-LIT-DATA: {"K": 0, "y": []}
data {
  int<lower=0> K;
  array[K] int y;
}
parameters {
  vector[K] mu;
  real sigma;
}
model {
  sigma ~ normal(0, 1);
  target += multinomial_lpmf(y | softmax(mu + sigma));
}
