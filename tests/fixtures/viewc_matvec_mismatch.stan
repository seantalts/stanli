data { matrix[2, 3] X; }
parameters { vector[2] b; }
model { target += sum(X * b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL multiply: Columns of m1 (3) and Rows of m2 (2) must match in size
// STANLI-LIT-DATA: {"X": [[1, 2, 3], [4, 5, 6]]}
