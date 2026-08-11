parameters {
  real q;
}
model {
  matrix[0, 3] Z = rep_matrix(0.0, 0, 3);
  matrix[0, 3] A = Z;
  target += cols(A) + q;
}
