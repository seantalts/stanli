// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: is given containers of different sizes under a condition that depends on a parameter
// As container_return_size_differs, but stanc inlines g, so the result is
// stanc's own unsized local.
functions {
  vector g(real c) {
    if (c > 0) return rep_vector(1, 2);
    return rep_vector(2, 3);
  }
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
  target += sum(g(-mu)) * mu;
}
