parameters { vector[1] a; vector[2] b; }
model { target += sum(a + b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Plus__: incompatible logical views
