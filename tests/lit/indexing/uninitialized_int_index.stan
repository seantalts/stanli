// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: array[uni, ...] index: accessing element out of range. index -2147483648 out of range; expecting index to be between 1 and 2
// Stan starts an int local at the minimum int, so n[k] reads index
// -2147483648, and CmdStan raises this out-of-range error at every
// evaluation. The print keeps stanc's --O1 from removing the statement.
parameters {
  real mu;
}
model {
  array[2] int n;
  int k = n[k];
  print("k = ", k);
  mu ~ normal(0, 1);
}
