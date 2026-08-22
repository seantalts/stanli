// Index bounds are data: every index form on a parameter-dependent value,
// with the index values supplied by the data. CmdStan rejects an
// out-of-bounds index at runtime; the graph lowering knows every index at
// bind time, so it must reject then -- silently reading a neighboring
// arena slot is the alternative. hi < lo ranges stay empty, not errors.
data {
  int k;
  array[2] int idx;
  int lo;
  int hi;
  int i1;
  int j1;
  int rl;
  int rh;
  int m;
  vector[4] Y;
  matrix[4, 2] Zm;
}
transformed data {
  array[3] real xs = {1.0, 2.0, 3.0};
  real tds = xs[m];
}
parameters {
  real mu;
}
model {
  vector[4] v = Y + rep_vector(mu, 4);
  matrix[4, 2] M = Zm + rep_matrix(mu, 4, 2);
  target += normal_lpdf(v[k] | 0, 1);
  target += normal_lpdf(v[idx] | 0, 1);
  target += normal_lpdf(v[lo:hi] | 0, 1);
  target += normal_lpdf(sum(M[i1]) | 0, 1);
  target += normal_lpdf(sum(M[ : , j1]) | 0, 1);
  target += normal_lpdf(sum(M[rl:rh]) | 0, 1);
  target += normal_lpdf(sum(M[rl:rh, j1]) | 0, 1);
  target += normal_lpdf(tds | 0, 1);
  target += normal_lpdf(mu | 0, 2);
}
