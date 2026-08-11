parameters { real q; vector[1] a; vector[2] b; }
model {
  vector[2] c = q > 0 ? a + b : b;
  target += sum(c);
}
