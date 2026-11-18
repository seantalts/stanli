// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 0}
// An empty array assigned under a branch whose condition prints. After the
// branch, lowering knows t only from the branch's result, so the result has
// to carry the array's shape for size(t) to read it. g is overloaded so
// that stanc does not inline it.
functions {
  real g(real a) {
    print("a=", a);
    return 1.32;
  }
  real g(int a) {
    return a;
  }
}
data {
  int<lower=0> N;
}
parameters {
  real p;
}
transformed parameters {
  array[N] real t = rep_array(-0.84, N);
  if (g(1.0) > p) t = inv_logit(rep_array(0.23, N));
  if (size(t) != N) reject("size ", size(t));
}
model {
  p ~ normal(0, 1);
}
