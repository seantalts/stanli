data { array[2] int idx; }
parameters { vector[6] q; }
model {
  matrix[3, 2] M = to_matrix(q, 3, 2);
  matrix[2, 2] A = M[1:2];
  matrix[2, 2] B = M[idx];
  target += dot_product(to_vector(A), [1, 2, 4, 8]')
            + dot_product(to_vector(B), [16, 32, 64, 128]');
}
