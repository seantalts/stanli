// Integer reads that only the caller can answer, in the compile-time
// integer positions inside a parameter-dependent region: an element of a
// data array at rank two, and a literal array's size and elements --
// which is the form stanc's inliner leaves behind when it substitutes an
// `array[] int` argument at a call site.
functions {
  array[,] int echo_int_matrix(array[,] int x) {
    return x;
  }
}
data {
  array[3, 2] int idx;
}
parameters {
  real theta;
}
model {
  if (theta > 0) {
    int total = 0;
    for (r in 1 : 3) {
      if (idx[r, 2] == 5) total += idx[r, 1];
    }
    int m = size({4, 5, 6}) + {4, 5, 6}[2];
    array[2] int row = idx[2, 1:2];
    array[3, 2] int copied = idx;
    copied[2, 1:2] = {9, 8};
    int echoed_rows = size(echo_int_matrix(copied));
    int echoed_cells = num_elements(echo_int_matrix(copied));
    target += (total + 10 * m + 100 * sum(row)
               + 1000 * sum(copied[2, 1:2]) + 10000 * echoed_rows
               + 100000 * echoed_cells) * theta;
  } else {
    target += theta;
  }
}
