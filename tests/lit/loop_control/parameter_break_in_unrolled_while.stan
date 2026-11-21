// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N1": 2}
// The inner while sizes a local, so stanli unrolls the outer one, whose
// break depends on a parameter. In CmdStan the loop runs once when
// p[1] <= 0.46 and three times otherwise.
data {
  int N1;
}
parameters {
  array[N1] real p;
}
model {
  int w = 0;
  while (w < 3) {
    w += 1;
    int v = 0;
    while (v < 0) {
      v += 1;
      array[size({0.5})] int l = {0};
    }
    if (p[1] <= 0.46) break;
  }
  if (w != (p[1] <= 0.46 ? 1 : 3)) reject("w is ", w);
  p ~ normal(0, 1);
  target += w;
}
