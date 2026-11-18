// min and max of two integers in an index, where one argument is a shape
// query of a parameter vector: the integer is known while lowering even
// though the expression is not data-only.
functions {
  real last_or_third(vector x) {
    return x[min(rows(x), 3)];
  }
}
data {
  int N;
}
parameters {
  vector[N] p;
}
model {
  target += p[max(1, rows(p) - 1)] + 2 * last_or_third(p);
}
