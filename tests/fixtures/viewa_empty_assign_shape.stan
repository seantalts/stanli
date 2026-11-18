functions {
  array[] matrix filled(int n, int r, int c) {
    array[n] matrix[r, c] b;
    for (i in 1:n) b[i] = rep_matrix(i, r, c);
    return b;
  }
}
parameters {
  real q;
}
model {
  // Each inlined return temporary is declared array[0] matrix[0, 0]; an
  // empty destination takes the shape of the value assigned to it.
  array[0] matrix[2, 3] none = filled(0, 2, 3);
  array[2] matrix[0, 3] flat = filled(2, 0, 3);
  array[2] matrix[0, 0] again;
  again = filled(2, 0, 3);
  target += dims(again)[3];
  again = filled(2, 4, 0);
  target += 10 * dims(again)[2];
  target += 2 * q + size(none) + 100 * dims(flat)[3] + 1000 * cols(flat[2]);
}
