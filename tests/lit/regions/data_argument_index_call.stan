// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 6, "D": [[0.0, 0.5, 1.25, 2.0, 3.5, 4.0], [0.5, 0.0, 0.75, 1.5, 3.0, 3.5], [1.25, 0.75, 0.0, 0.75, 2.25, 2.75], [2.0, 1.5, 0.75, 0.0, 1.5, 2.0], [3.5, 3.0, 2.25, 1.5, 0.0, 0.5], [4.0, 3.5, 2.75, 2.0, 0.5, 0.0]]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=75 slots=125
// STANLI-LIT-CHECK-NOT: INDEX
// A call that reads a data matrix through a formal without the data
// qualifier folds the reads like its inlined body in
// data_argument_index_inline.stan; keep their checks identical. The overload
// keeps stanc from inlining weighted_squares.
functions {
  real weighted_squares(matrix x, real a) {
    real s = 0;
    for (i in 1:rows(x))
      for (j in 1:cols(x))
        s += a * square(x[i, j]);
    return s;
  }
  real weighted_squares(real x) {
    return x;
  }
}
data {
  int N;
  matrix[N, N] D;
}
parameters {
  real<lower=0> a;
}
model {
  target += -weighted_squares(D, a);
}
