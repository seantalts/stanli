// Integer `%` and `%/%`, and the matrix solves `A \ v` and `rv / A`, in
// generated quantities. Every one of them has to produce CmdStan's value
// through whichever path the section lands on -- the solves became graph
// ops (runtime/kernels/matrix_fns.cpp) and the integer operators fold, so
// today that is the graph. What this pins is that the columns are all
// there: when an operator is missing from the path the section takes,
// column discovery throws at every probe point and the driver reports the
// constrained parameters as the whole CSV, so the generated quantities
// disappear with no error anywhere. Same shapes as
// tests/stanc3/operators.stan, which is where the gap was found.
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
