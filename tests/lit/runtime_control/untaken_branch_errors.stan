// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"K": 0, "N": 2}
// The branch never runs at the check point, so neither the division by
// zero nor the size mismatches may stop the model from loading.
data {
  int K;
  int N;
}
parameters {
  real theta;
}
model {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  array[2] vector[3] av = {v, v};
  matrix[2, 2] m = rep_matrix(0, 2, 2);
  if (theta > 100) {
    int k = 5 %/% K;
    target += k;
    target += sum(v[1:(5 %/% K)]);
    v = rep_vector(1, N);
    v[1:2] = rep_vector(1, N + 1);
    v[2:] = rep_vector(1, N + 1);
    v[{1, 2}] = rep_vector(1, N + 1);
    a[1:2] = rep_array(1.0, N + 1);
    av[:] = {v};
    m[1:2, 1] = rep_vector(1, N + 1);
  }
  target += sum(v) + sum(a) + sum(av[1]) + sum(m);
  theta ~ normal(0, 1);
}
