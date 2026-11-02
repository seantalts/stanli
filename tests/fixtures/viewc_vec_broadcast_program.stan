parameters { real q; vector[1] a; vector[2] b; }
model {
  vector[2] c = q > 0 ? a + b : b;
  target += sum(c);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: add: a (1, 1) and b (2, 1) must match in size
