parameters { row_vector[1] a; row_vector[2] b; }
model { target += sum(a + b); }
