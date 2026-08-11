functions {
  array[] matrix passthrough(data array[] matrix A) { return A; }
}
data { array[2] matrix[2, 3] A; }
parameters { real q; }
model {
  array[2] matrix[2, 3] B = passthrough(A);
  target += q + B[1, 1, 1] + 10 * B[1, 2, 1]
            + 100 * B[1, 1, 2] + 1000 * B[1, 2, 2]
            + 10000 * B[1, 1, 3] + 100000 * B[1, 2, 3]
            + 1000000 * B[2, 1, 1] + 10000000 * B[2, 2, 1]
            + 100000000 * B[2, 1, 2] + 1000000000 * B[2, 2, 2]
            + 10000000000.0 * B[2, 1, 3] + 100000000000.0 * B[2, 2, 3];
}
generated quantities {
  array[2] matrix[2, 3] C = passthrough(A);
}
