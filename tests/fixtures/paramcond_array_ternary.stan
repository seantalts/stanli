// A conditional expression on a parameter whose arms are arrays: an array
// of reals and an array of vectors, each arm the same shape.
parameters {
  real p;
}
model {
  array[2] real l = p < 0.25 ? {1.0, 2.0} : {p, 3.0};
  array[2] vector[2] m = p < 0.25 ? {[p, 1]', [2, 3]'} : {[1, 2]', [3, p]'};
  target += l[1] + l[2] + m[1][1] + 2 * m[2][2];
}
