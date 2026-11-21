// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"v0": 1.5, "i0": 2.5}
// CmdStan counts a scalar as zero elements when it compares sizes, so a
// scalar satisfies a declaration with a zero extent and is read as empty.
data {
  vector[0] v0;
  array[0] int i0;
}
transformed data {
  if (num_elements(v0) != 0 || size(i0) != 0) reject("zero-size data is not empty");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
