data { matrix[2, 3] X; }
parameters { vector[2] b; }
model { target += sum(X * b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Times__: inner dimension mismatch
// STANLI-LIT-DATA: {"X": [[1, 2, 3], [4, 5, 6]]}
