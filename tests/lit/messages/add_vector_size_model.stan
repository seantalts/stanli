// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: add: m1 (3, 1) and m2 (4, 1) must match in size
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
