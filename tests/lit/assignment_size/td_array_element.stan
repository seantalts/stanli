// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign rows: assigning variable a (3) and right hand side rows (2) must match in size
transformed data {
  array[2] vector[3] a = {[1, 2]', [3, 4]'};
}
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
