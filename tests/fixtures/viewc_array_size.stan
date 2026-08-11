functions {
  real score(real q) {
    array[2, 3] real A = {{1, 2, 3}, {4, 5, 6}};
    return 100 * size(A) + num_elements(A) + q;
  }
}
parameters { real q; }
model { target += score(q); }
