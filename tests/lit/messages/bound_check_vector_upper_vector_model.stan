// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: v[2] is 3, but must be less than or equal to 2.000000
parameters {
  real y;
}
transformed parameters {
  vector<upper=rep_vector(2, 3)>[3] v = rep_vector(1 + y * 0, 3);
  v[2] = 3 + y * 0;
}
model {
  y ~ normal(0, 1);
}
