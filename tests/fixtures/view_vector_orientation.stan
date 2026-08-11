parameters {
  real q;
}
model {
  vector[3] v = rep_vector(q, 3);
  row_vector[3] r = to_row_vector(v);
  vector[0] v0 = rep_vector(0.0, 0);
  row_vector[0] r0 = to_row_vector(v0);

  target += q
            + rows(v) + 10 * cols(v)
            + 100 * rows(r) + 1000 * cols(r)
            + 10000 * rows(v0) + 100000 * cols(v0)
            + 1000000 * rows(r0) + 10000000 * cols(r0);
}
