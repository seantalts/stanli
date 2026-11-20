// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// `lo:hi` with hi < lo, and `:hi` with hi < 1, select nothing on vectors
// and arrays alike, whatever the endpoints.
parameters {
  vector[3] v;
  array[3] real a;
}
model {
  target += num_elements(v[:0]) + num_elements(a[:-2]) + num_elements(v[5:4])
            + num_elements(a[5:4]) + sum(v[:0]) + sum(a[5:4]);
  v ~ normal(0, 1);
  a ~ normal(0, 1);
}
