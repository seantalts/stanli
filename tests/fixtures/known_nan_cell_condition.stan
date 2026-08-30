functions {
  matrix fill_declared_nans(matrix A) {
    int n = rows(A);
    matrix[n, n] out;
    for (j in 1:n) {
      for (i in 1:n) {
        if (i == j)
          out[i, j] = A[i, j];
        if (is_nan(out[i, j]))
          out[i, j] = 0;
      }
    }
    return out;
  }
}

parameters {
  matrix[3, 3] A;
  real theta;
}

model {
  // Keep the complete UDF in one necessary runtime-control region so its
  // declaration and every cell guard share one ProgramCompiler value state.
  if (theta > -100)
    target += theta * sum(fill_declared_nans(A));
  else
    target += theta;
}
