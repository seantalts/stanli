parameters { vector[1] a; vector[2] b; }
model { target += sum(a + b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL add: m1 (1, 1) and m2 (2, 1) must match in size
