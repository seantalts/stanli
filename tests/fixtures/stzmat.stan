parameters {
  sum_to_zero_matrix[4, 5] m;
}
model {
  target += sum(m .* m);
}
