// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector[uni] indexing: accessing element out of range. index 0 out of range; expecting index to be between 1 and 3
// STANLI-LIT-DATA: {"k": 0}
data {
  int k;
}
parameters {
  vector[3] y;
}
model {
  target += y[k];
}
