parameters { row_vector[2] r; matrix[1, 3] M; }
model { target += sum(append_row(r, M)); }
