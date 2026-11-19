// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// f_lpdf asks for the normalized density, so g_lupdf inside it keeps its
// constants: cauchy_lpdf(0.5 | 1.38, 0.5) is -1.86198. The call sits under
// a branch on a parameter, so a runtime region inlines f, and the value it
// folds for g has to use f's normalization. stanc does not inline
// overloaded functions.
functions {
  real g_lpdf(real y) {
    return cauchy_lupdf(0.5 | 1.38, 0.5);
  }
  real g_lpdf(vector y) {
    return 0;
  }
  real f_lpdf(real y) {
    return g_lupdf(0.5 | );
  }
  real f_lpdf(vector y) {
    return 0;
  }
}
parameters {
  real p;
}
model {
  real m = 0;
  if (p < 100) m = f_lpdf(p | );
  if (m > -1.86 || m < -1.87) reject("m ", m);
  p ~ normal(0, 1);
}
