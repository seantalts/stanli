parameters { real q; }
model {
  array[3, 2] real A = {{q, 2}, {3, 4}, {5, 6}};
  array[2, 3] real B;
  // A second definition, so O1 cannot copy-propagate `B` away and the
  // mismatched assignment survives into the MIR.
  for (i in 1 : 2) {
    B = A;
  }
  target += B[1, 1];
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL assignment logical view mismatch for B
