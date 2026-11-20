// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N0": 2}
// From fuzzer issue #617. stanc inlines f13, so its returns become breaks
// out of a one-trip loop around nested loops. CmdStan runs it with lp 0.
functions {
  vector f4() {
    return [0.50, 0.75]';
  }
  real f13() {
    for (e15 in f4()) {
      for (i16 in 1:2) {
        if ((e15 <= -0.24)) {
          return 0.25;
        }
      }
    }
    return 0.50;
  }
}
data {
  int<lower=0> N0;
}
model {
  if (f13() != 0.50) reject("f13() gave ", f13());
}
