data {
  vector[3] d;
  array[3] int counts;
}
parameters {
  real pad;
  vector[5] x;
}
model {
  target += prod(d);
  target += sum(counts);
  pad ~ normal(0, 1);
  x ~ normal(0, 1);
}
generated quantities {
  real pr = prod(x);
  array[5] int z;
  array[2] int tail;
  array[2, 2] int partial_matrix;
  partial_matrix[1, 1] = 7;
  z[1] = 1;
  z[2] = bernoulli_rng(pr);
  z[3] = 0;
  tail[1] = 0;
  tail[2] = bernoulli_rng(0.7);
  z[10 : 0] = tail[2 : 1];
  z[4 : 5] = tail;
  int total = sum(z);
}
