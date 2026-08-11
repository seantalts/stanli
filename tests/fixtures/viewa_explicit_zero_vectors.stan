functions {
  real shape_code(array[] vector x) {
    return 100 * size(x) + 10 * num_elements(x) + dims(x)[2];
  }
}
parameters {
  real q;
}
model {
  array[2] vector[0] values = {rep_vector(0, 0), rep_vector(0, 0)};
  target += q + shape_code(values);
}
