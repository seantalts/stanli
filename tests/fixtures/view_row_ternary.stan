parameters {
  real q;
}
model {
  row_vector[3] a = [1.0, 2.0, q];
  row_vector[3] b = [4.0, 5.0, q];
  row_vector[3] r = q > 0 ? a : b;
  target += 100 * rows(r) + 10 * cols(r) + sum(r);
}
