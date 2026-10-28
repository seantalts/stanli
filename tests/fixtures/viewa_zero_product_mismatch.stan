data {
  array[0] vector[0] empty;
}
parameters {
  real q;
}
model {
  array[2] vector[0] values = empty;
  // A second definition, so O1 cannot copy-propagate `values` away and the
  // mismatched assignment survives into the MIR.
  for (i in 1 : 2) {
    values = empty;
  }
  target += q + size(values);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL assignment logical view mismatch for values
// STANLI-LIT-DATA: {"empty": []}
