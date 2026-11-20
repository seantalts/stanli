// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign rows: assigning variable v (3) and right hand side rows (2) must match in size
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
    v = rep_vector(1, N);
  }
  target += sum(v) + sum(a);
  theta ~ normal(0, 1);
}
