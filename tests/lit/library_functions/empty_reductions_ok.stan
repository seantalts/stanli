// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 0, "d": []}
// sum, prod, log_sum_exp, min and max of an empty real container return a
// value. sd, mean and variance reject one, but only when they run: here
// they sit in branches that the data or the parameter keeps from running.
data {
  int N;
  vector[N] d;
}
parameters {
  real t;
  vector[N] v;
}
model {
  t ~ normal(0, 1);
  target += sum(v) + prod(v) + prod(d) + sum(d);
  target += (min(v) > max(v)) + (log_sum_exp(v) < 0);
  if (t > 10) target += sd(v) + mean(v) + variance(v);
  if (N > 1) target += sd(d) + mean(d) + variance(d);
}
