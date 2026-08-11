parameters { real q; }
model {
  row_vector[0] a = to_row_vector(rep_vector(0.0, 0));
  row_vector[0] b = to_row_vector(rep_vector(0.0, 0));
  row_vector[0] r = q > 0 ? a : b;
  target += 100 * rows(r) + cols(r) + q;
}
