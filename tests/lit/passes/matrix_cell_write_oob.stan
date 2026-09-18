// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: out of bounds
// STANLI-LIT-DATA: {"R": 3, "C": 4, "x": [[0.1, 0.2, 0.3, 0.4], [0.5, 0.6, 0.7, 0.8], [0.9, 1.0, 1.1, 1.2]], "idx": [3, 1]}
data {
  int R;
  int C;
  matrix[R, C] x;
  array[2] int idx;
}
parameters {
  real mu;
  vector[2] b;
}
model {
  matrix[R, C] m = x + mu;
  m[5, 1] = mu;
  target += sum(m);
  mu ~ normal(0, 1);
  b ~ normal(0, 1);
}
