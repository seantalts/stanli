// A shape query on a scalar `int` input, standing alone on the right of a
// REAL-valued assignment. Lowering answers rows/cols/size from the graph's
// slot metadata or from the declaration table, and bind_data fills both
// from a declared shape -- which a scalar int does not have. So a scalar
// int is the one name that reaches the data-map fallback below those two.
//
// Two things have to line up for it to get there. The context has to be
// real: in an int context stanc3 marks the expression data_only and the
// MIR interpreter answers it, which is why the transformed-data line here
// kept working the whole time the transformed-parameters one did not. And
// the call has to be the whole right-hand side: written `size(n) + 1` the
// constant folder takes the assignment before the shape query is reached.
data {
  int n;
}
transformed data {
  // Int context: answered by the interpreter, and never broken.
  int td_n = size(n);
}
parameters {
  real y;
}
transformed parameters {
  // Real context, alone: the fallback this fixture exists for. `size(n)`
  // is 1 whatever n holds, so a wrong answer moves the mean by whole
  // units rather than by a rounding.
  real p = size(n);
}
model {
  y ~ normal(p + td_n, 1);
}
