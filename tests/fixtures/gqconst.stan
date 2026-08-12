// A generated quantity the optimizer folds to a constant: at --O1 the
// FnWriteParam's argument becomes the literal itself, so the column name
// must come from output_vars instead of the (gone) variable reference.
parameters {
  real x;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  real z;
  z = 3;
}
