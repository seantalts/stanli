// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL complex numbers are not supported yet (in variable w)
parameters {
  real mu;
}
model {
  complex_vector[2] w;
  w[1] = to_complex(1, 2);
  mu ~ normal(0, 1);
}
