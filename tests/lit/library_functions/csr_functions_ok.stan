// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  array[4] int v = {1, 3, 2, 3};
  array[4] int u = {1, 3, 4, 5};
  vector[4] wd = [1, 2, 3, 4]';
  vector[3] bd = [1, -1, 2]';
  matrix[3, 3] dense = [[1, 0, 2], [0, 3, 0], [0, 0, 4]];
  if (max(abs(to_vector(csr_to_dense_matrix(3, 3, wd, v, u) - dense))) > 1e-15)
    reject("csr_to_dense_matrix");
  if (max(abs(csr_matrix_times_vector(3, 3, wd, v, u, bd) - dense * bd)) > 1e-14)
    reject("csr_matrix_times_vector");
}
parameters {
  vector[4] w;
  vector[3] b;
}
model {
  matrix[3, 3] d = csr_to_dense_matrix(3, 3, w, v, u);
  matrix[3, 3] expected = [[w[1], 0, w[2]], [0, w[3], 0], [0, 0, w[4]]];
  vector[3] product = csr_matrix_times_vector(3, 3, w, v, u, b);
  if (max(abs(to_vector(d - expected))) > 1e-14) reject("csr_to_dense_matrix param");
  if (max(abs(product - expected * b)) > 1e-12)
    reject("csr_matrix_times_vector param");
  target += -0.5 * dot_self(w) - 0.5 * dot_self(b) + sum(d * d);
}
