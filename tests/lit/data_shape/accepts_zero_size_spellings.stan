// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"Z": [[], [], []], "A0": [[]], "A2": [], "v0": [], "i0": []}
// Any empty JSON array satisfies a declaration with a zero extent, whatever
// its nesting.
data {
  matrix[0, 3] Z;
  array[0] vector[3] A0;
  array[2] vector[0] A2;
  vector[0] v0;
  array[0] int i0;
}
transformed data {
  if (rows(Z) != 0 || cols(Z) != 3 || size(A0) != 0 || size(A2) != 2
      || num_elements(A2) != 0 || num_elements(v0) != 0)
    reject("zero-size data has the wrong shape");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
