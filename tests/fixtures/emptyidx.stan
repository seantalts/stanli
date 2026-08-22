// An empty int data array as a gather index (issue #133 family): R's
// integer(0) serializes to JSON [], which must stay integer-typed. The
// density consumer makes the empty case exercise the kernels' activity
// masks, and the generated quantity walks the write_array path.
data {
  int<lower=0> M;
  array[M] int idx;
  vector[4] Y;
}
parameters {
  real mu;
}
model {
  target += normal_lpdf(Y[idx] | mu, 1);
  target += normal_lpdf(mu | 0, 2);
}
generated quantities {
  real gsum = sum(Y[idx]);
}
