functions {
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
