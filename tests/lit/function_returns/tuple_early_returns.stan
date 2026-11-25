// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// A function that returns a tuple from more than one place, one of them in a
// loop, under conditions on a parameter. The overload keeps stanc from
// inlining it, so stanli rewrites the returns itself.
functions {
  void expect(real got, real want, int id) {
    if (got != want) reject("case ", id, ": got ", got, ", want ", want);
  }
  tuple(real, vector) rescale(vector v, real s) {
    if (s > 2) return (s, v / s);
    for (i in 1:rows(v))
      if (v[i] > 3 * s) return (0.0, v);
    return (s, v * s);
  }
  real rescale(real x) {
    return x;
  }
  void check_values(real p) {
    real s = 0.5 + inv_logit(p);
    tuple(real, vector[2]) r = rescale([0.1, 0.2]', s);
    expect(r.1, s, 1);
    expect(r.2[2], 0.2 * s, 2);
    r = rescale([0.1, 0.2]', s + 2);
    expect(r.1, s + 2, 3);
    expect(r.2[2], 0.2 / (s + 2), 4);
    r = rescale([1, 10 * s]', s);
    expect(r.1, 0, 5);
    expect(r.2[2], 10 * s, 6);
  }
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
  check_values(p);
  tuple(real, vector[2]) r = rescale([p, 1]', p);
  target += r.1 + sum(r.2);
}
generated quantities {
  check_values(p);
}
