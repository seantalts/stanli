// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: g
// An integer density of an empty array is zero. The recursive function,
// inlined inside loops, sends this generated quantity to the draw-time
// interpreter, which must accept the empty outcome array.
functions {
  real rate(int n) {
    if (n <= 0) {
      return 0.5;
    }
    return rate(n - 1);
  }
  real f_lpmf(array[] int y) {
    for (e in rep_array(0, 0)) {
      for (x in rep_row_vector(0.0, 0)) {
        return 0.5;
      }
    }
    return poisson_log_lpmf(y | tanh(rate(min(3, 2))));
  }
}
generated quantities {
  real g = f_lpmf(rep_array(0, 0) | );
}
