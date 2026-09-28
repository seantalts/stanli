functions {
  vector affine(vector x, real a, real b) {
    return a * x + b;
  }
  int plus_one(int x) {
    return x + 1;
  }
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
  vector dynamic_result(real x) {
    if (x > 0) return rep_vector(x, 2);
    return rep_vector(x, 3);
  }
  real observed(real x) {
    print("seen:", x);
    if (x < 0) reject("bad:", x);
    return x + 1;
  }
  void noop() { return; }
  real unseeded_rng() { return normal_rng(0, 1); }
  int integer_divide(int x, int y) { return x %/% y; }
  int integer_modulo(int x, int y) { return x % y; }
  vector combined(real x) {
    vector[3] a = affine([x, x+1, x-2]', 2.5, -1);
    matrix[2,2] m = scale_matrix([[x,2],[3,x+1]], 0.5);
    vector[2] b = sized(x, 2);
    return [a[1],a[2],a[3],branch_exit(40*x),m[1,1],m[2,1],
            m[1,2],m[2,2],b[1],b[2]]';
  }
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities { vector[10] output = combined(x); }
