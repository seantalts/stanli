functions {
  vector matrix_gather(real t, vector y, matrix A, matrix B) {
    int i = y[1] > 0 ? 1 : 2;
    array[2] int selected = {i, i};
    matrix[2, 3] C = A;
    C[selected, {2, 1}] = A[{2, 1}, {1, 2}];
    row_vector[3] row = C[i];
    matrix[2, 3] gathered = C[selected, ];
    return [row[2] + gathered[1, 1] * y[1],
            gathered[2, 2] * y[2] + B[1, 3]]';
  }
  vector matrix_bounds(real t, vector y, matrix A, matrix B) {
    int i = t < 0 ? 0 : t > 1 ? 3 : 1;
    return [A[i, 2] * y[1], B[2, 3] * y[2]]';
  }

  vector nested_shape(real t, vector y, array[,] vector A) {
    return [size(A) + y[1], size(A[1]) + y[2]]';
  }
  vector nested_values(real t, vector y, array[] matrix A,
                       array[,] real B, array[,] int tags) {
    return [A[2, 1, 3] * y[1] + B[2, 3] + tags[2, 1],
            A[1, 2, 1] * y[2] - B[1, 2] + tags[1, 2]]';
  }

  vector matrix_helper(vector y, matrix A, matrix B) {
    return [A[1, 2] * y[1] + B[2, 3], A[2, 3] * y[2] - B[1, 2]]';
  }
  vector matrix_early(real t, vector y, matrix A, matrix B) {
    if (t > 0.5) return matrix_helper(y, A, B);
    return [A[2, 1] * y[2] + B[1, 3], A[1, 3] * y[1] - B[2, 2]]';
  }
  vector matrix_shape(real t, vector y, matrix A) {
    return [rows(A) + y[1], cols(A) + y[2]]';
  }
  vector matrix_dynamic(real t, vector y, matrix A, matrix B) {
    array[y[1] > 0 ? 1 : 2] real temporary;
    temporary[1] = y[1];
    print("matrix callback effect");
    return [A[1, 2] * temporary[1] + B[2, 3], A[2, 3] * y[2] - B[1, 2]]';
  }
}
model {}
