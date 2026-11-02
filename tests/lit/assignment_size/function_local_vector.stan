// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign rows: assigning variable yp (1) and right hand side rows (4) must match in size
// STANLI-LIT-DATA: {"x": [1, 2, 3, 4]}
functions {
  vector scaled(vector y) {
    // stanc --O1 leaves yp Uninit, so CmdStan built with --O1 takes the
    // four elements; plain stanc sizes yp and throws, as Stan's semantics
    // say.
    vector[1] yp = -2 * y;
    return yp;
  }
}
data {
  vector[4] x;
}
transformed data {
  vector[4] r = scaled(x);
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
