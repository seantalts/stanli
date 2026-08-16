// Two-argument log_sum_exp / log_diff_exp are elementwise with scalar
// broadcast in Stan, not reductions: every container form of the argument
// pair returns a container of the same shape. The graph lowering used to
// emit both at width 1 whatever the operands were, so the value never fit
// the variable it was assigned to.
//
// Every call here is assigned to a declared result rather than reduced in
// place, because that is where the too-narrow value was caught, and it is
// the shape the conformance sweep reports: `--O1` inlines a user function
// and parks the call's value in a sized temporary.
functions {
  real pair_sum(vector hi, vector lo) {
    vector[3] r = log_diff_exp(hi, lo);
    return sum(r);
  }
}
data {
  array[3] int counts;
  matrix[2, 3] mat;
}
parameters {
  vector[3] a;
  real b;
}
model {
  vector[3] hi = a + 8;
  // vector x vector, through the inliner's sized temporary.
  target += pair_sum(hi, a);
  // Scalar broadcast on either side; the scalar picks up the other
  // operand's logical view rather than collapsing the result to width 1.
  vector[3] vs = log_diff_exp(hi, b);
  target += sum(vs);
  row_vector[3] sr = log_sum_exp(b, hi');
  target += sum(sr);
  // An int container argument widens to a real result.
  array[3] real ia = log_sum_exp(counts, b);
  target += sum(ia);
  // A matrix leaf: the declaration only accepts a 2x3 logical view, so
  // this is where a result that kept the width but lost the extents dies.
  matrix[2, 3] mr = log_diff_exp(mat + 8, b);
  target += sum(mr);
  // A nested array: the shape the sweep exercises up to eight deep.
  array[2] vector[3] na = log_sum_exp(b, {hi, a});
  target += sum(na[1]) + sum(na[2]);
}
