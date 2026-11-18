// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// A tuple returned from inside a while loop, with a call in the returned
// expression. stanc does not inline an overloaded function, so stanli's
// region compiler inlines it, and that replaced the compiler's variable
// table while the assignment still held an iterator into it.
functions {
  row_vector inner() {
    return [0.16];
  }
  row_vector inner(real x) {
    return [x];
  }
  tuple(real, row_vector) outer() {
    int w = 0;
    while (w < 1) {
      w += 1;
      return (0.5, inner());
    }
    return (0.23, [0.48]);
  }
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  tuple(real, row_vector[1]) t = outer();
  if (t.1 != 0.5 || t.2[1] != 0.16) reject("got ", t.1, ", ", t.2);
}
