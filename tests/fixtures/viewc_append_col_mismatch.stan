parameters { vector[2] a; vector[3] b; }
model { target += sum(append_col(a, b)); }
