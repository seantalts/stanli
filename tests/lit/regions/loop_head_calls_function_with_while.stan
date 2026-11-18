// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Stan evaluates a for loop's upper bound and a while loop's condition
// before every trip. Each evaluation of f runs its own while loop, so its
// counter w has to start from 0 every time. stanc does not inline an
// overloaded function, so stanli inlines f into each loop's head.
functions {
  real f() {
    int w = 0;
    real acc = 0;
    while (w < 1) {
      w += 1;
      acc += 3;
    }
    if (acc != 3) reject("acc ", acc);
    return acc;
  }
  real f(real x) {
    return x;
  }
}
parameters {
  real p;
}
model {
  real n = 0;
  while (n < f() + 0 * p) {
    n += 1;
  }
  for (i in 1:to_int(f())) {
    n += 1;
  }
  if (n != 6) reject("n ", n);
  p ~ normal(0, 1);
}
