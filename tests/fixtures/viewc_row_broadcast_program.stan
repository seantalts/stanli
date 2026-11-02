parameters { real q; row_vector[1] a; row_vector[2] b; }
model {
  row_vector[2] c = q > 0 ? a + b : b;
  target += sum(c);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: add: m1 (1, 1) and m2 (1, 2) must match in size
