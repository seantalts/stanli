parameters { matrix[2, 2] M; vector[3] v; }
model { target += sum(append_col(M, v)); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL append_col row mismatch
