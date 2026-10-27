// log_prob lowering fails while write_array lowers cleanly.
parameters { row_vector[2] a; row_vector[3] b; }
model { target += sum(append_row(a, b)); }
generated quantities { real s = sum(a) + sum(b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL append_row column mismatch
