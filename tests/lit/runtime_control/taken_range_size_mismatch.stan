// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector[min_max] assign: assigning variable v (2) and right hand side (3) must match in size
// STANLI-LIT-DATA: {"N": 2}
data {
  int N;
}
parameters {
  real theta;
}
model {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  if (theta > -100) {
    v[1:2] = rep_vector(1, N + 1);
  }
  target += sum(v) + sum(a);
  theta ~ normal(0, 1);
}
