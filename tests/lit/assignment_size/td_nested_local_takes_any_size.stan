// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"b0": [1, 2, 3, 4, 5, 6, 7]}
data {
  vector[7] b0;
}
transformed data {
  {
    // A block-local, unlike a transformed data variable, is an empty Eigen
    // object in CmdStan's C++ and takes the 7 elements unchecked.
    vector[8] nested = b0;
    print(sum(nested));
  }
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
