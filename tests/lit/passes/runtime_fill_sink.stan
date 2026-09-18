// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: FORWARD:
// STANLI-LIT-CHECK-NOT: FILL
// STANLI-LIT-CHECK: SQUARE
// STANLI-LIT-CHECK: FILL
functions {
  real conditional_series(real x) {
    vector[128] terms;
    int k = 1;
    if (x > 0) return square(x);
    terms[1] = x;
    while (k < 3 && x < 0) {
      k += 1;
      terms[k] = k * x;
    }
    return log_sum_exp(terms[1:k]);
  }
}
parameters { real x; }
model { target += conditional_series(x); }
