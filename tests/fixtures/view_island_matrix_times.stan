parameters {
  real q;
}
model {
  matrix[2, 2] A = to_matrix([1.0, q, 3.0, 4.0]', 2, 2);
  matrix[2, 2] B = to_matrix([5.0, 6.0, 7.0, 8.0]', 2, 2);
  matrix[2, 2] C = q > 0 ? A * B : B * A;
  target += C[1, 1];
}
