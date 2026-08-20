// The named-function spellings of Stan's binary operators -- add,
// subtract, multiply, elt_multiply, divide, elt_divide -- and
// squared_distance, which is not one of them but rides the same two
// kernels.
//
// Every call appears twice: once on data in transformed data, where the
// MIR interpreter answers, and once on parameters in the model block,
// where the graph kernels do. The transformed-data results are summed
// into the target rather than left in a declaration, because that is
// what makes a disagreement between the two halves move lp. Teaching the
// graph lowering and not the interpreter is how a vectorized
// log_sum_exp once reported 2.6252 where the answer was 2.9302, with no
// exception anywhere.
data {
  vector[2] dv;
  row_vector[2] drv;
  matrix[2, 2] dm;
}
transformed data {
  real td_acc = sum(add(dv, 0.5)) + sum(add(dv, dv))
                + sum(subtract(dv, 0.25)) + sum(subtract(0.75, dv))
                + sum(multiply(dm, dv)) + multiply(drv, dv)
                + sum(multiply(dv, drv)) + sum(multiply(dm, dm))
                + sum(elt_multiply(dv, dv)) + sum(elt_multiply(2.0, dv))
                + sum(divide(dv, 2.0)) + sum(divide(1.5, dm))
                + sum(elt_divide(dv, dv)) + sum(elt_divide(1.0, dv))
                + squared_distance(dv, drv) + squared_distance(dv[1], dv[2]);
}
parameters {
  vector[2] p;
  real q;
}
model {
  target += td_acc;
  // Elementwise, both broadcast directions, all four container shapes.
  target += sum(add(p, q));
  target += sum(add(p, dv));
  target += sum(subtract(q, p));
  target += sum(elt_multiply(p, p));
  target += sum(elt_multiply(q, p));
  target += sum(divide(p, q));
  target += sum(elt_divide(q, p));
  target += sum(divide(q, dm));
  // multiply's linear algebra: matvec against a data matrix, inner
  // product, outer product.
  target += sum(multiply(dm, p));
  target += multiply(p', p);
  target += sum(multiply(p, p'));
  // squared_distance pairs a vector with a row_vector in the language,
  // which the elementwise view check would reject.
  target += squared_distance(p, drv);
  target += squared_distance(p', dv);
  target += squared_distance(q, p[1]);
  // A region whose control flow depends on a parameter compiles to the
  // register program or not at all, so the aliases have to be there too.
  if (q > -100.0) {
    target += add(p[1], q) - divide(p[2], q);
  }
}
