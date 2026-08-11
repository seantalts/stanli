parameters {
  vector[6] q;
}
model {
  matrix[2, 3] A = to_matrix(q, 2, 3);
  matrix[2, 3] B = to_matrix(-q, 2, 3);
  matrix[2, 3] M = q[1] > 0 ? A : B;

  target += M[1, 1] + 2 * M[2, 3];
  target += sum(M * [1.0, 2.0, 3.0]');
  target += sum((q[1] > 0 ? A : B) * [4.0, 5.0, 6.0]');
}
