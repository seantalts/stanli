// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"d": [1e16, 1, -1e16, 1]}
// Stan Math's mean of autodiff values sums them in order, and of data in
// Eigen's packets: CmdStan gives 0.25 and 0.5 here.
data {
  vector[4] d;
}
parameters {
  real t;
}
transformed parameters {
  vector[4] y = t * 0 + d;
}
model {
  if (mean(y) != 0.25) reject("mean(y) is ", mean(y));
  if (mean(d) != 0.5) reject("mean(d) is ", mean(d));
  t ~ normal(0, 1);
}
