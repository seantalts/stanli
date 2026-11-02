parameters { vector[1] a; vector[2] b; }
model { target += sum(a + b); }

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL add: a (1, 1) and b (2, 1) must match in size
