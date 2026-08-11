data {
  array[2, 3] real A;
}
parameters {
  real q;
}
model {
  target += q
            + A[1, 1] + 10 * A[1, 2] + 100 * A[1, 3]
            + 1000 * A[2, 1] + 10000 * A[2, 2] + 100000 * A[2, 3];
}
