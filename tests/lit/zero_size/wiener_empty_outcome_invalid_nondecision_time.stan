// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: wiener_lpdf: Nondecision time is -1, but must be positive finite!
// STANLI-LIT-DATA: {"tau": -1}
data {
  vector[0] y;
  real tau;
}
parameters {
  real<lower=0.5, upper=2> a;
  real<lower=0.2, upper=0.8> b;
  real d;
}
model {
  y ~ wiener(a, tau, b, d);
  d ~ normal(0, 1);
}
