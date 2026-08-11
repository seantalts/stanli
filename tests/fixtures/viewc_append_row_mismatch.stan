parameters { row_vector[2] a; row_vector[3] b; }
model { target += sum(append_row(a, b)); }
