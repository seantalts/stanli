functions {
  array[] matrix pass_through(array[] matrix x) {
    return x;
  }
  real shape_code(array[] matrix x, real q) {
    return q + 100 * dims(x)[1] + 10 * dims(x)[2] + dims(x)[3]
           + 1000 * size(x) + 10000 * num_elements(x);
  }
}
data {
  array[0] matrix[2, 3] A;
}
parameters {
  real q;
}
model {
  target += shape_code(pass_through(A), q);
}
generated quantities {
  array[0] matrix[2, 3] B = pass_through(A);
  real proof = shape_code(B, 0);
}
