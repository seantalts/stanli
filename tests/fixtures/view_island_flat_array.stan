parameters {
  real q;
}
model {
  array[2, 3] real A = {{q, q + 1, q + 2}, {q + 3, q + 4, q + 5}};
  array[3, 2] real B = {{q, q + 1}, {q + 2, q + 3}, {q + 4, q + 5}};
  array[2, 3] real C = q > 0 ? A : B;
  target += C[1, 1];
}
