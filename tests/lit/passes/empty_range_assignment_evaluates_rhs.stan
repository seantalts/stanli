// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: Scale parameter
parameters {
  real y;
}
model {
  vector[1] x = rep_vector(0, 1);
  x[1 : 0] = rep_vector(normal_lpdf(y | 0, -1), 0);
  target += x[1];
  y ~ normal(0, 1);
}
