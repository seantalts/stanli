// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// A foreach over a ternary whose condition calls a function. stanc does
// not inline overloaded functions, so stanli's region compiler inlines both
// calls. Inlining g replaced the table of locals that wait for their first
// assignment to fix their shape, while the assignment held an iterator
// into it.
functions {
  real g(real a) { return 0.27; }
  real g(int a) { return -1; }
  real f(real a) {
    real s = 0;
    for (e in (g(a) > -0.26) ? [1, 2]' : [3, 4]') s += e;
    return s;
  }
  real f(int a) { return a; }
}
parameters { real p; }
model {
  real x = 0;
  if (f(p) < 100) x = f(p);
  if (x != 3) reject("got ", x);
}
