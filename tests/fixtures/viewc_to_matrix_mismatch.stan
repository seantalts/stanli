parameters { vector[6] q; }
model {
  matrix[2, 4] M = to_matrix(q, 2, 4);
  target += sum(M);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL stanli compile: to_matrix: to_matrix(matrix): rows * columns (8) and vector size (6) must match in size
