functions {
  real vector_shape(array[] vector x, real q) {
    return q + 1000 * size(x) + 100 * num_elements(x)
           + 10 * dims(x)[1] + size(dims(x));
  }
  real row_vector_shape(array[] row_vector x, real q) {
    return q + 1000 * size(x) + 100 * num_elements(x)
           + 10 * dims(x)[1] + size(dims(x));
  }
  real matrix_shape(array[] matrix x, real q) {
    return q + 10000 * size(x) + 1000 * num_elements(x)
           + 100 * dims(x)[1] + size(dims(x));
  }
  real scalar_array_shape(array[,] real x, real q) {
    return q + 1000 * size(x) + 100 * num_elements(x)
           + 10 * dims(x)[1] + size(dims(x));
  }
}
data {
  array[0] vector[4] vectors;
  array[0] row_vector[5] rows;
  array[0] matrix[2, 3] matrices;
  array[0, 3] real scalars;
}
parameters {
  real q;
}
model {
  target += vector_shape(vectors, q) + row_vector_shape(rows, q)
            + matrix_shape(matrices, q) + scalar_array_shape(scalars, q);
}
