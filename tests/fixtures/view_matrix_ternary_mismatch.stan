parameters {
  vector[6] q;
}
model {
  matrix[2, 3] A = to_matrix(q, 2, 3);
  matrix[3, 2] B = to_matrix(-q, 3, 2);
  target += sum(row(q[1] > 0 ? A : B, 1));
}
