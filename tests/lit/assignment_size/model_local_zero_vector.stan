// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// stan::model::assign lets a zero-size destination take the right-hand side's size.
parameters {
  real y;
}
model {
  vector[0] v;
  v = rep_vector(y, 3);
  target += sum(v);
  y ~ normal(0, 1);
}
