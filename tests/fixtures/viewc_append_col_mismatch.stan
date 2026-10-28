parameters { vector[2] a; vector[3] b; }
model { target += sum(append_col(a, b)); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL append_col row mismatch
