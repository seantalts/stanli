// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL modulus: divisor is 0
// STANLI-LIT-DATA: {"divisor":0}
functions {
  real remainder_check(real x, int divisor) {
    int n = 7;
    if (x > 1000) return x;
    if (x <= 0) n = -7;
    return x + n % divisor;
  }
}
data { int divisor; }
parameters { real x; }
model { target += remainder_check(x, divisor); }
