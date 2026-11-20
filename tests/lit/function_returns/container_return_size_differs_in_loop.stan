// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: the result of g is given containers of different sizes
// As container_return_size_differs, with the first return inside a loop.
functions {
  vector g(real c) {
    for (i in 1:2) {
      if (c * i > 0) return rep_vector(1, 2);
    }
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
  target += sum(g(-mu)) * mu;
}
