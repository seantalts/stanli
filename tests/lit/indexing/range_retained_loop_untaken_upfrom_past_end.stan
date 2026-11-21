// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"K": 5}
data { int K; }
parameters { real theta; }
model {
  vector[3] v = [1, 2, 3]';
  array[3] real a = {1, 2, 3};
  real s = 0;
  for (i in 1:40) {
    if (theta > i + 100) s += sum(v[K:]) + sum(a[K + 1:]);
  }
  target += -s;
  theta ~ normal(0, 1);
}
