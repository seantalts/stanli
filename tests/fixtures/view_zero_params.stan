parameters {
  matrix[0, 3] P03;
  real q;
  matrix[3, 0] P30;
  vector[2] b;
  matrix[0, 0] P00;
  real tail;
}
model {
  target += rows(P03) + 10 * cols(P03)
            + 100 * rows(P30) + 1000 * cols(P30)
            + 10000 * rows(P00) + 100000 * cols(P00)
            + q + 2 * b[1] + 4 * b[2] + 8 * tail;
}
