// The cross-path harness's own canary: the smallest model on which the
// graph and the MIR interpreter can disagree silently.
//
// `B / A` with a matrix divisor is a linear solve, and stanc3 spells it
// with the ordinary division operator, so nothing in the MIR name says so
// -- only the divisor's type does. mir_interp.hpp has always keyed on the
// type; lower.cpp keyed on the operator until #129, which made the same
// line mean a solve in the interpreter and elementwise division in the
// graph. Shapes line up either way, so the only symptom was wrong numbers.
//
// Kept deliberately to that one shape. `solve.stan` covers all four solve
// shapes, but two of them were LOUD failures before #129, so it cannot
// compile against the tree the bug lived in and cannot demonstrate that
// this harness would have caught it. This one compiles on both sides.
//
// Q is a transformed parameter so both write_array engines produce it:
// the graph lowers the whole section, and STANLI_WA_FORCE_INTERP attaches
// the interpreter beside it.
data {
  int N;
  matrix[N, N] A;
}
parameters {
  matrix[N, N] B;
}
transformed parameters {
  matrix[N, N] Q = B / A;
}
model {
  target += sum(Q);
}
