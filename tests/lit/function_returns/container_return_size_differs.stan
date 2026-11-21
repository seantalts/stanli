// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: the result of g is given containers of different sizes
// Stan lets a function return containers of different sizes from different
// returns. When the branch depends on a parameter, stanli has one fixed
// size for the result, so it refuses the model while loading it. It must
// not choose a size and reject the points where the other return runs.
// The second call takes the second return at the default point.
functions {
  vector g(real c) {
    if (c > 0) return rep_vector(1, 2);
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
