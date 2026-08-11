parameters { vector[9] q; }
model {
  real s = q[1];
  vector[2] v = q[2:3];
  row_vector[2] r = q[4:5]';
  matrix[2, 2] M = to_matrix(q[6:9], 2, 2);
  vector[3] ar1 = append_row(s, v);
  vector[3] ar2 = append_row(v, s);
  row_vector[3] ac1 = append_col(s, r);
  row_vector[3] ac2 = append_col(r, s);
  matrix[3, 2] mr = append_row(M, r);
  matrix[2, 3] mv = append_col(M, v);
  target += dot_product(ar1, [1, 2, 3]')
          + dot_product(ar2, [4, 5, 6]')
          + dot_product(ac1, [7, 8, 9]')
          + dot_product(ac2, [10, 11, 12]')
          + dot_product(to_vector(mr), [13, 14, 15, 16, 17, 18]')
          + dot_product(to_vector(mv), [19, 20, 21, 22, 23, 24]');
}
