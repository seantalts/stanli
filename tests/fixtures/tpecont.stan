// `target += <container>` for every container shape. Stan defines
// `target += e` for a container `e` as adding `sum(e)`, not e[1].
data {
  int<lower=0> N;
  vector[N] w;
}
transformed data {
  vector[N] w2 = 2 * w;
  real ws = sum(w2);
}
parameters {
  vector[3] v;
  row_vector[3] r;
  matrix[2, 2] M;
  array[2] real a;
}
model {
  target += v;
  target += r;
  target += M;
  target += a;
  target += 2 * v;  // a container expression, not just a bare variable
}
generated quantities {
  real total = ws + sum(v) + sum(r) + sum(M) + sum(a);
}
