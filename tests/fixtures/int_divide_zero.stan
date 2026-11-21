// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL divide: denominator is 0
// STANLI-LIT-DATA: {"z": 0}
data {
  int z;
}
transformed data {
  int q = 1 %/% z;
}
parameters {
  real theta;
}
model {
  theta ~ std_normal();
}
