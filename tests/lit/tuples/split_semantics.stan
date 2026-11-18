// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Each check rejects when splitting tuples into leaf variables changes a
// value: assignments that read the leaves they overwrite, a while condition,
// a right operand and a ternary branch that call a tuple function, a size
// computed from one, and a foreach over an array of tuples. The overloads
// keep the tuple functions from being inlined.
functions {
  tuple(real, int) adv(real x) { return (x - 1, x > 0); }
  tuple(real, int) adv(vector x) { return (x[1] - 1, x[1] > 0); }
  tuple(int, real) sz(real x) { return (3, x); }
  tuple(int, real) sz(vector x) { return (2, x[1]); }
  void check(int ok, data array[] int where) {
    if (!ok) reject("split changed a value at ", where);
  }
  void semantics(real mu) {
    tuple(real, real) t = (mu, 2 * mu);
    t = (t.2, t.1);
    check(t.1 == 2 * mu && t.2 == mu, {1});
    array[2] tuple(real, int) arr = {(1.5, 2), (2.5, 3)};
    arr = {arr[2], arr[1]};
    check(arr[1].1 == 2.5 && arr[1].2 == 3 && arr[2].1 == 1.5, {2});
    arr[1] = arr[2];
    check(arr[1].1 == 1.5 && arr[1].2 == 2, {3});
    real fe = 0;
    for (e in arr) fe += e.1 * e.2;
    check(fe == 6, {4});
    real x = 3;
    int n = 0;
    while (adv(x).2 && n < 10) {
      n += 1;
      x = adv(x).1;
      if (n == 2) continue;
    }
    check(n == 3 && x == 0, {5});
    check(((n > 3) && adv(x).2) == 0, {6});
    check(((n > 2) || adv(x).2) == 1, {7});
    check((n > 2 ? adv(x + 5).1 : 0) == 4, {8});
    vector[sz(1.5).1] v = rep_vector(sz(2.5).2, sz(0.5).1);
    check(sum(v) == 7.5, {9});
  }
}
transformed data {
  semantics(0.5);
}
parameters {
  real mu;
}
model {
  semantics(mu);
  mu ~ normal(0, 1);
}
generated quantities {
  semantics(0.5);
}
