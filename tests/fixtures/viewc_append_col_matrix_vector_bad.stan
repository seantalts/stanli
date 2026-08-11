parameters { matrix[2, 2] M; vector[3] v; }
model { target += sum(append_col(M, v)); }
