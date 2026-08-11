data {
  matrix[2, 3] M;
}
transformed data {
  row_vector[3] r = row(M, 2);
}
generated quantities {
  real picked = M[2, 3];
  real shaped = 100 * rows(M) + 10 * cols(M)
                + 1000 * rows(r) + cols(r) + r[2];
}
