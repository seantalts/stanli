// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"s": 2, "a1": [2.0], "v1": [3], "M": [[1, 2, 3], [4, 5, 6]], "av": [[1, 2, 3], [4, 5, 6]], "a23": [[1, 2, 3], [4, 5, 6]], "v3": [1, 2, 3], "rv3": [1, 2, 3], "ar3": [1, 2, 3], "n": 4, "ai": [1, 2], "m11": [[1.0]]}
// Flat lists for vector, row_vector and array of real; integer tokens for real
// data; nested rows for matrices and arrays of vectors.
data {
  real s;
  array[1] real a1;
  vector[1] v1;
  matrix[2, 3] M;
  array[2] vector[3] av;
  array[2, 3] real a23;
  vector[3] v3;
  row_vector[3] rv3;
  array[3] real ar3;
  int n;
  array[2] int ai;
  matrix[1, 1] m11;
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
