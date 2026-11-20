// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: wiener_lpdf: A-priori bias is 1.5, but must be in the interval [0, 1]
// STANLI-LIT-DATA: {"beta": 1.5}
data {
  vector[0] y;
  real beta;
}
parameters {
  real<lower=0.5, upper=2> a;
  real<lower=0, upper=0.1> t;
  real d;
}
model {
  target += wiener_lpdf(y | a, t, beta, d);
  d ~ normal(0, 1);
}
