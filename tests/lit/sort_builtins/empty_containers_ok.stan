// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  array[0] real a;
  array[0] int b;
  vector[0] v;
  row_vector[0] r;
  array[0] real sa = sort_asc(a);
  array[0] int sb = sort_desc(b);
  vector[0] sv = sort_asc(v);
  row_vector[0] sr = sort_desc(r);
  array[0] int ia = sort_indices_asc(a);
  array[0] int ib = sort_indices_desc(v);
}
parameters { real y; }
model { y ~ normal(0, 1); }
