functions {
  real vector_min(vector x) {
    array[2] matrix[2, 2] uninlined_shape_guard;
    return min(x);
  }
}
parameters {
  vector[5] x;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  real x_min = vector_min(x);
}
