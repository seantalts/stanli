functions {
  real containers(real q) {
    matrix[2, 3] m = [[q, 0.2, 0.3], [0.4, q + 0.5, 0.6]];
    array[3] vector[2] columns = to_vector_array(m);
    array[2] row_vector[3] rows_ = to_row_vector_array(m);
    array[3] vector[2] s = softmax(columns);
    array[2] row_vector[3] l = log_softmax(rows_);
    matrix[2, 3] a = to_matrix(columns);
    matrix[2, 3] b = to_matrix(rows_);
    return a[2, 1] + 2 * a[1, 3] + b[2, 2]
           + s[1, 1] + 2 * s[2, 2] + 3 * s[3, 1]
           + l[1, 3] + 2 * l[2, 1];
  }
}
transformed data {
  real data_result = containers(0.15);
  matrix[0, 3] zr;
  matrix[2, 0] zc;
  array[3] vector[0] ac = to_vector_array(zr);
  array[2] row_vector[0] ar = to_row_vector_array(zc);
  array[0] vector[2] empty;
  array[0] vector[2] soft_empty = softmax(empty);
  if (rows(to_matrix(ac)) != 0 || cols(to_matrix(ac)) != 3 ||
      rows(to_matrix(ar)) != 2 || cols(to_matrix(ar)) != 0)
    reject("zero extent conversion");
}
parameters { real q; }
transformed parameters {
  real result = containers(q);
  real branch_result;
  if (q > 0) branch_result = containers(q + 0.1);
  else branch_result = containers(q - 0.1);
}
model { target += result + branch_result + data_result; }
generated quantities { real output_result = containers(q); }
