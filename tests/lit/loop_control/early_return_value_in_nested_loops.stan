// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N0": 2}
// As early_return_in_nested_loops, returning the loop's own element. No
// element is negative, so f returns 0, as in CmdStan.
functions {
  vector g() { return [0.5, 0.75]'; }
  real f() {
    for (e in g()) {
      for (i in 1:2) {
        if (e < 0) return e;
      }
    }
    return 0;
  }
}
data {
  int N0;
}
model {
  if (f() != 0) reject("f() gave ", f());
}
