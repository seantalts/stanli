// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// stanc names the temporary and the index of each foreach loop `tmp1__` and
// `idx1__`, also for a loop nested in another, where the inner names hide
// the outer ones. Every sum is 1 * 3 + 2 * 3 = 9 only if the outer loop
// keeps its own array and index.
functions {
  real nested_sum() {
    real s = 0;
    for (a in {1, 2}) {
      for (b in [3.0]) {
        s += a * b;
      }
    }
    return s;
  }
}
transformed data {
  real td = 0;
  for (a in {1, 2}) {
    for (b in [3.0]) {
      td += a * b;
    }
  }
  if (td != 9) reject("transformed data: got ", td);
}
parameters {
  real p;
}
transformed parameters {
  real tp = 0;
  for (a in {1, 2}) {
    for (b in {3.0}) {
      tp += a * b;
    }
  }
}
model {
  real m = 0;
  for (a in {1, 2}) {
    for (b in [3.0]) {
      m += a * b;
    }
  }
  if (m != 9) reject("model: got ", m);
  p ~ normal(0, 1);
}
generated quantities {
  real gq = 0;
  for (a in [1.0, 2.0]) {
    for (b in {3}) {
      gq += a * b;
    }
  }
  if (tp != 9) reject("transformed parameters: got ", tp);
  if (gq != 9) reject("generated quantities: got ", gq);
  if (nested_sum() != 9) reject("function: got ", nested_sum());
}
