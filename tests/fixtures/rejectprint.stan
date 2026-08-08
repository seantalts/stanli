// reject() and print(), in both places they can appear: transformed data
// (evaluated eagerly by the MIR interpreter, where a reject must stop the
// model from compiling at all) and the model block (lowered to an op,
// where a reject must throw at evaluation and count as a rejected draw).
data { int N; real lim; }
transformed data {
  if (N < 0) reject("N must be nonnegative, got ", N);
  print("compiled with N = ", N);
}
parameters { real x; vector[2] v; }
model {
  if (N > 100) reject("N too large: ", N, " limit ", lim);
  x ~ normal(0, 1);
  v ~ normal(0, 1);
}
