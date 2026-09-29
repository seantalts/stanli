functions {
  vector affine(vector x, real a, real b) {
    return a * x + b;
  }
  int plus_one(int x) {
    return x + 1;
  }
  real promote_integer(int x) { return 1.5 * x; }
  real promoted_return(int x) { return x + 10; }
  real real_identity(real x) {
    return x;
  }
  matrix scale_matrix(matrix x, real a) {
    return a * x;
  }
  real overloaded(real x) {
    return x + 0.5;
  }
  real overloaded(vector x) {
    return sum(x);
  }
  real numeric(int x) {
    return x + 10;
  }
  real numeric(real x) {
    return x + 20;
  }
  real direction(vector x) {
    return sum(x);
  }
  real direction(row_vector x) {
    return -sum(x);
  }
  real descend(real x, int remaining) {
    if (remaining == 0) return x;
    return descend(x + 1, remaining - 1);
  }
  real branch_exit(real x) {
    if (x > 0) return 2 * x;
    while (x < -1) {
      if (x < -2) return -3 * x;
      return -4 * x;
    }
    return x + 7;
  }
  vector sized(real x, int n) {
    vector[n] y;
    for (i in 1:n) y[i] = x + i;
    return y;
  }
  array[] real array_identity(array[] real x) { return x; }
  array[] vector nested_identity(array[] vector x) { return x; }
  array[,] real nested_scalars(array[,] real x, real a) {
    array[size(x), size(x[1])] real y = x;
    for (i in 1:size(x))
      for (j in 1:size(x[i])) y[i, j] = a * x[i, j] + 100 * i + j;
    return y;
  }
  array[,] int nested_integers(array[,] int x) { return x; }
  array[] row_vector nested_rows(array[] row_vector x, real a) {
    array[size(x)] row_vector[cols(x[1])] y = x;
    for (i in 1:size(x))
      for (j in 1:cols(x[i])) y[i, j] = a * x[i, j] + 100 * i + j;
    return y;
  }
  array[] matrix nested_matrices(array[] matrix x, real a) {
    array[size(x)] matrix[rows(x[1]), cols(x[1])] y = x;
    for (i in 1:size(x))
      for (r in 1:rows(x[i]))
        for (c in 1:cols(x[i]))
          y[i, r, c] = a * x[i, r, c] + 100 * i + 10 * r + c;
    return y;
  }
  array[,] matrix nested_matrix_identity(array[,] matrix x) { return x; }
  int nested_length(array[] vector x, real gate) {
    int i = gate > 0 ? 1 : 3;
    return num_elements(x[i]);
  }
  vector dynamic_result(real x) {
    if (x > 0) return rep_vector(x, 2);
    return rep_vector(x, 3);
  }
  real observed(real x) {
    print("seen:", x);
    if (x < 0) reject("bad:", x);
    return x + 1;
  }
  real guarded_constructor(real x, int n) {
    print("checked:", x);
    if (x > 0) return sum(linspaced_vector(n, 0, 1));
    return x;
  }
  void noop() { return; }
  real unseeded_rng() { return normal_rng(0, 1); }
  int integer_divide(int x, int y) { return x %/% y; }
  int integer_modulo(int x, int y) { return x % y; }
}
model {}
