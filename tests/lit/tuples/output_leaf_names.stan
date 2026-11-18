// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-WANAMES: p.1,p.2,leading:2,trailing_empty:1,after_trailing:1,nested_empty:1,by_element:1,by_element:2:1.1.1,by_element:2:1.2.1,by_element:2:2,swapped:1,swapped:2,copied:2.1,copied:2.2,copy_of_later:1,later,unwritten_leaf:3,in_array.1:1,in_array.1:2,in_array.2:1,in_array.2:2
// The optimizer turns the writes of these tuple outputs into literals or
// into reads of other variables, so the write no longer names its leaf. The
// names must still come out as for the unoptimized program: an empty leaf
// before or after a written one, empty leaves nested in a tuple, an array of
// vectors written one element at a time, a leaf copied from another leaf of
// the same tuple, a leaf copied from a parameter, a leaf copied from a
// later output, an empty leaf that gets no write, and an array of tuples.
functions {
  tuple(real, tuple(matrix, matrix)) nested() {
    return (0.5, (rep_matrix(0.75, 0, 0), rep_matrix(1.0, 0, 0)));
  }
  tuple(int, tuple(array[] vector, int)) elements() {
    return (0, ({[0.75]', [1.25]'}, 0));
  }
  tuple(real, row_vector) trailing() {
    return (-0.28, rep_row_vector(0.0, 0));
  }
}
parameters {
  vector[2] p;
}
model {
  p ~ normal(0, 1);
}
generated quantities {
  tuple(array[0] real, int) leading = (rep_array(1.14, 0), 1);
  tuple(real, row_vector[0]) trailing_empty = trailing();
  tuple(int, matrix[0, 0]) after_trailing = (2, rep_matrix(0.0, 0, 0));
  tuple(real, tuple(matrix[0, 0], matrix[0, 0])) nested_empty = nested();
  tuple(int, tuple(array[2] vector[1], int)) by_element = elements();
  tuple(real, real) swapped = (-0.21, 0.04);
  swapped = (swapped.2, -0.15);
  tuple(vector[0], vector[2]) copied = (rep_vector(0.0, 0), p);
  tuple(real, array[0] real) copy_of_later = (0.58, rep_array(0.83, 0));
  real later = 0.5;
  copy_of_later.1 = later;
  tuple(array[2] vector[0], vector[0], real) unwritten_leaf;
  unwritten_leaf.2 = rep_vector(0.0, 0);
  unwritten_leaf.3 = p[1];
  array[2] tuple(real, int) in_array;
  for (i in 1:2) in_array[i] = (p[1] * i, i);
}
