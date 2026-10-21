data {
  int N;
  array[N] int yi;
  vector[N] etad;
  array[N] ordered[2] cd;
}
parameters {
  real z;
}
model {
  z ~ normal(0, 1);
  yi ~ ordered_logistic(etad, cd);
}
