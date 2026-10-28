parameters { row_vector[2] a; row_vector[3] b; }
model { target += sum(append_row(a, b)); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL append_row column mismatch
