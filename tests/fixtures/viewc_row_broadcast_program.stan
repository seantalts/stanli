parameters { real q; row_vector[1] a; row_vector[2] b; }
model {
  row_vector[2] c = q > 0 ? a + b : b;
  target += sum(c);
}
