// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"a": [[1.5, 0, 0, 0], [0, 3, 0, 0], [0, 0, 0, 5]], "b": [1, 2, 3, 4]}
data {
  matrix[3, 4] a;
  vector[4] b;
}
parameters {
  real y;
}
model {
  vector[3] r = csr_matrix_times_vector(3, 4, csr_extract_w(a),
                                        csr_extract_v(a), csr_extract_u(a), b);
  vector[3] w = csr_extract_w(a);
  y ~ normal(sum(r) + sum(w), 1);
}
