// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: assignment width mismatch for the result of g
// Stan lets a function return containers of different sizes from different
// returns. When the branch depends on a parameter, stanli has one fixed
// size for the result, so it refuses the model while loading it. It must
// not choose a size and compute a wrong value.
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
  target += sum(g(mu)) * mu;
}
