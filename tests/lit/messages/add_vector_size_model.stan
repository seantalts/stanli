// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: add: a (3, 1) and b (4, 1) must match in size
// STANLI-LIT-DATA: {"N": 3, "M": 4}
data {
  int N;
  int M;
}
parameters {
  vector[N] a;
  vector[M] b;
}
model {
  target += sum(a + b);
}
