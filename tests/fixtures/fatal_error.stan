data {
  int N;
}
transformed data {
  if (N < 0) fatal_error("N is ", N);
}
parameters {
  real x;
}
model {
  x ~ normal(0, 1);
  if (N > 100) fatal_error("N too large: ", N);
  if (x > 10) fatal_error("x too large: ", x);
}
