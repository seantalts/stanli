// The four linear-solve shapes stanc3 spells with the division operators,
// and the two divisions that must stay elementwise. `A` is data and `P`
// carries parameters, so the divisor adjoint is exercised as well as the
// dividend's. The same solves appear in generated quantities, where every
// operand is a double and the section must still lower to the graph.
data {
  int N;
  matrix[N, N] A;
}
parameters {
  matrix[N, N] B;
  row_vector[N] rv;
  vector[N] v;
  matrix[N, 2] W;
}
transformed parameters {
  matrix[N, N] P = A + B;
}
model {
  target += sum(B / A);
  target += sum(rv / A);
  target += sum(A \ v);
  target += sum(A \ W);
  target += sum(rv / P);
  target += sum(P \ W);
  target += sum(B / 2.0);
  target += sum(B ./ A);
}
generated quantities {
  vector[N] gv = A \ v;
  row_vector[N] grv = rv / A;
}
