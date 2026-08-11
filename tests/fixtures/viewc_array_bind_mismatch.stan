parameters { real q; }
model {
  array[3, 2] real A = {{q, 2}, {3, 4}, {5, 6}};
  array[2, 3] real B;
  B = A;
  target += B[1, 1];
}
