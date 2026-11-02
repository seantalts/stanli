// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: v[3][2] is nan, but must be greater than or equal to 0.000000
parameters {
  real y;
}
transformed parameters {
  array[3] vector<lower=0>[2] v;
  for (i in 1:3) {
    v[i] = rep_vector(1 + y * 0, 2);
  }
  v[3][2] = not_a_number() + y * 0;
}
model {
  y ~ normal(0, 1);
}
