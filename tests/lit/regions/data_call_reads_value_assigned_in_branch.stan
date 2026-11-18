// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// g's argument is data, so the region compiler can ask the lowering outside
// the branch for the call's value. That lowering still holds v from before
// the branch, so the call has to be compiled in the branch. g is overloaded
// so that stanc does not inline it.
functions {
  real g(vector v) {
    return v[1];
  }
  real g(real v) {
    return v;
  }
}
parameters {
  real p;
}
model {
  vector[1] v = [0.5]';
  real s = 0;
  if (p < 100) {
    v[1] = -0.74;
    s = g(v);
  }
  if (s != -0.74) reject("got ", s);
}
