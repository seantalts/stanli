functions {
  array[] matrix filled(int n, int r, int c) {
    array[n] matrix[r, c] b;
    for (i in 1:n) b[i] = rep_matrix(i, r, c);
    return b;
  }
}
parameters {
  real q;
}
transformed parameters {
  // A destination with elements in its outer extent still checks it.
  array[2] matrix[0, 0] two;
  two = filled(3, 1, 1);
}
model {
  target += q + size(two);
}
