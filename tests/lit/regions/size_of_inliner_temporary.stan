// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// stanc's inliner declares the argument temporary for a12 with size 0 and
// the branch assigns it two vectors, so size(a12) inside f is 2 and f
// returns [0.5, 0.75]. A runtime region must not take the size from the
// declaration.
functions {
  row_vector f(array[] vector a12) {
    if (0.5 + size(a12) * 0.5 > 1) {
      return [0.5, 0.75];
    }
    return [-0.73, -0.48];
  }
}
parameters {
  real p;
}
model {
  vector[2] g = p < 100 ? f({[0.28, 0.53]', [0.78, 1.03]'})' : [-0.93, -0.68]';
  if (g[1] != 0.5) reject("g ", g[1]);
  p ~ normal(0, 1);
}
