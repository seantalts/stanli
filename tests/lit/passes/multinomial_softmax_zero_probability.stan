// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"p": [0.25, 0, 0.75], "y": [3, 0, 2]}
// A zero probability makes a logit -inf. multinomial_lpmf accepts it when
// its count is zero; multinomial_logit_lpmf would reject the logit.
data {
  vector[3] p;
  array[3] int y;
}
parameters {
  real sigma;
}
model {
  sigma ~ normal(0, 1);
  target += multinomial_lpmf(y | softmax(log(p) + sigma));
}
