// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"mx": 2147483647}
// stanc compiles Stan's int operators to C++ int arithmetic, where overflow
// is undefined: CmdStan prints mx + 1 as -2147483648 but its compiler also
// assumes that mx + 1 cannot equal it, so this model rejects there. Every
// stanli evaluator wraps the same way: the register program in the branch
// on a parameter, lowering's folding before it, and the interpreter in
// generated quantities.
functions {
  void check(int mx, int folded) {
    if (mx + 1 != -2147483647 - 1) reject("mx + 1 is ", mx + 1);
    if (mx * 2 != -2) reject("mx * 2 is ", mx * 2);
    if (-(-mx - 1) != -2147483647 - 1) reject("-(-mx - 1) is ", -(-mx - 1));
    if (folded * 2 != -2) reject("2147483647 * 2 is ", folded * 2);
  }
}
data {
  int mx;
}
parameters {
  real mu;
}
model {
  if (mu > -100) check(mx, 2147483647);
  check(mx, 2147483647);
  mu ~ normal(0, 1);
}
generated quantities {
  int gq = 0;
  check(mx, 2147483647);
}
