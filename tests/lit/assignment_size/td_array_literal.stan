// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: assign array size: assigning variable a (3) and right hand side (2) must match in size
transformed data {
  array[3] int a = {1, 2};
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
