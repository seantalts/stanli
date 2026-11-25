// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"b": 6}
// A function's local that has no initial value is NaN, also when a data
// variable has the same name (stanc3's compound-assign/plus_equals_prim).
// Each check rejects when the local reads the data instead. The overload
// keeps stanc from inlining foo.
functions {
  real foo(real a1) {
    real b;
    b += a1;
    return b;
  }
  real foo(vector a1) {
    return 0;
  }
}
data {
  int b;
}
transformed data {
  if (!is_nan(foo(1.0))) reject("transformed data: foo(1) is ", foo(1.0));
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  real z = foo(2.0);
  if (!is_nan(z)) reject("generated quantities: foo(2) is ", z);
}
