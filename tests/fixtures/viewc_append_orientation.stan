parameters { vector[10] q; }
model {
  row_vector[2] r2 = [q[1], q[2]];
  row_vector[3] r3 = [q[3], q[4], q[5]];
  vector[2] v2 = [q[6], q[7]]';
  vector[3] v3 = [q[8], q[9], q[10]]';
  row_vector[5] r = append_col(r2, r3);
  vector[5] v = append_row(v2, v3);
  target += 100 * rows(r) + 10 * cols(r) +
            1000 * rows(v) + cols(v) +
            dot_product(r, [1, 2, 4, 8, 16]') +
            dot_product(v, [32, 64, 128, 256, 512]);
}
