// Named aliases share one flat parameter slot but carry different logical
// views. Keeping all four names live catches compilers that attach matrix
// shape to the slot instead of to the binding.
data {
  matrix[2, 2] A;
  vector[2] y;
}
parameters {
  vector[6] q;
}
transformed parameters {
  matrix[2, 3] W = to_matrix(q, 2, 3);
}
model {
  matrix[2, 3] M = W;
  row_vector[6] r = to_row_vector(M);
  vector[6] v = to_vector(M);
  matrix[2, 2] D = A;
  matrix[2, 2] E = A;
  matrix[2, 2] B = A;
  M[1, 2] = M[1, 2] + v[6];
  D[1, 2] = q[1];
  B[1, 2] = sum(row(A, 1));
  if (q[1] > 0) {
    E = A * q[1];
  } else {
    E = A * -q[1];
  }

  target += dot_product(row(M, 1), segment(v, 1, 3));
  target += dot_product(r, to_row_vector(q));
  target += dot_product(v, q);
  target += sum(D * head(q, 2));
  target += sum(E * head(q, 2));
  y ~ normal_id_glm(B, 0, head(q, 2), 1);
}
