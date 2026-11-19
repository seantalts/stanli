// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Calling f_lpdf (not f_lupdf) asks for the normalized density, so the
// cauchy_lupdf inside it keeps its constants: cauchy_lpdf(0.5 | 1.38, 0.5)
// is -1.86198. The early return makes lowering rewrite the body, and the
// value folded for the local must use the caller's normalization too.
// stanc does not inline an overloaded function, so stanli inlines it.
functions {
  real f_lpdf(real y) {
    if (0.5 < 0.24) return 0.5;
    return cauchy_lupdf(0.5 | 1.38, 0.5);
  }
  real f_lpdf(vector y) {
    return 0;
  }
}
parameters {
  real p;
}
model {
  real m = f_lpdf(0.5 | );
  if (m > -1.86 || m < -1.87) reject("m ", m);
  p ~ normal(0, 1);
}
