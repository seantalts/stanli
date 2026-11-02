// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"x": [1, 2, 3, 4]}
functions {
  vector scaled(vector y) {
    // stanc --O1 leaves yp Uninit because its first use assigns all of it,
    // and CmdStan's empty Eigen local then takes the 4 elements unchecked.
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
