parameters {
  real q;
}
model {
  array[2] vector[3] vs = {[q, 2.0, 3.0]', [4.0, 5.0, 6.0]'};
  array[2] row_vector[3] rs = {[q, 2.0, 3.0], [4.0, 5.0, 6.0]};
  vector[3] v = vs[1];
  row_vector[3] r = rs[2];
  target += 100 * rows(v) + 10 * cols(v) + sum(v)
            + 1000 * rows(r) + 100 * cols(r) + sum(r);
}
