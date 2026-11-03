// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"dc": [[0.2, 0.5, 0.1], [0.8, 0.5, 0.9]], "dr": [[[0.2, 0.8], [0.5, 0.5]], [[0.1, 0.9], [0.3, 0.7]]]}
data {
  column_stochastic_matrix[2, 3] dc;
  array[2] row_stochastic_matrix[2, 2] dr;
}
parameters {
  column_stochastic_matrix[3, 2] pc;
  row_stochastic_matrix[2, 3] pr;
  array[2, 2] row_stochastic_matrix[2, 2] pa;
}
transformed parameters {
  column_stochastic_matrix[3, 2] tc = pc;
  array[2, 2] row_stochastic_matrix[2, 2] ta = pa;
}
model {
  target += 2 * pc[1, 1] + 3 * pr[2, 3] + 5 * pa[2, 1, 2, 2];
}
generated quantities {
  row_stochastic_matrix[2, 3] gr = pr;
}
