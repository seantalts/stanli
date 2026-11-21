// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL variance: v has size 0, but must have a non-zero size
// STANLI-LIT-DATA: {"N": 0}
// Stan Math's std::vector<var> overload names its argument v; the Eigen
// overloads and the data array say m.
data {
  int N;
}
parameters {
  real t;
  array[N] real v;
}
model {
  t ~ normal(0, 1);
  target += variance(v);
}
