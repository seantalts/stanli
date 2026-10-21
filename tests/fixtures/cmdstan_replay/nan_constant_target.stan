parameters {
  real a;
}
transformed parameters {
  real tp = not_a_number();
}
model {
  a ~ normal(1, 1);
  target += 1;
}
