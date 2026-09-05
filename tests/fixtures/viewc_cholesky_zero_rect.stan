model {
  matrix[0, 3] M = rep_matrix(0.0, 0, 3);
  target += sum(cholesky_decompose(M));
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL stanli compile: cholesky_decompose: needs a square matrix
