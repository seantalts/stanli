functions {
  real select_branch(real q) {
    array[2] matrix[2, 3] x = {
      [[1, 2, 3], [4, 5, 6]],
      [[7, 8, 9], [10, 11, 12]]
    };
    if (x[2][1, 2] == 8) return q;
    return 100 * q;
  }
}
parameters { real q; }
model {
  target += select_branch(q);
}
