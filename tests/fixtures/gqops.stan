// Integer `%` and `%/%`, and the matrix solves `A \ v` and `rv / A`, in
// generated quantities. None of them are graph ops, so the section falls
// back to the per-draw interpreter -- which has to know them too. When it
// does not, column discovery throws at every probe point and the driver
// reports the constrained parameters as the whole CSV: the generated
// quantities disappear with no error anywhere, which is the one outcome a
// caller cannot detect. Same shapes as tests/stanc3/operators.stan, which
// is where the gap was found.
data {
  int N;
}
parameters {
  vector[N] v;
  row_vector[N] rv;
  matrix[N, N] A;
}
model {
  target += normal_lpdf(v | 0, 1);
  target += normal_lpdf(rv | 0, 1);
  target += normal_lpdf(to_vector(A) | 0, 1);
}
generated quantities {
  int i = 7 % N;
  int q = 7 %/% N;
  vector[N] dv = A \ v;
  row_vector[N] drv = rv / A;
}
