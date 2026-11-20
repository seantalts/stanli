// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: the result of g is given containers of different sizes
// As container_return_size_differs, with an empty first return. stanli must
// not read the empty result as the other return's size.
functions {
  vector g(real c) {
    if (c > 0) return rep_vector(1, 0);
    return rep_vector(2, 3);
  }
  real g(vector x) {
    return 0;
  }
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
  target += sum(g(mu)) * mu + sum(g(-mu));
}
