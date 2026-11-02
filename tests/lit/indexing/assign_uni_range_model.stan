// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: array[uni,...] assign: accessing element out of range. index 6 out of range; expecting index to be between 1 and 4
// STANLI-LIT-DATA: {"k": 6}
data {
  int k;
}
parameters {
  real y;
}
model {
  array[4] real a = rep_array(y, 4);
  a[k] = 1;
  target += sum(a);
}
