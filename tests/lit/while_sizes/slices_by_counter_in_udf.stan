// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 6, "K": 2, "y": [1, 2, 3, 4, 5, 6]}
functions {
  array[] vector to_vector_of_arrays(vector vec, int length_array) {
    array[num_elements(vec) %/% length_array] vector[length_array] res;
    int j = 1;
    int i = 1;
    while (j <= num_elements(vec)) {
      res[i,] = vec[j : (j + length_array - 1)];
      i = i + 1;
      j = j + length_array;
    }
    return res;
  }
}
data {
  int N;
  int K;
  vector[N] y;
}
parameters {
  vector[N] mu;
}
model {
  array[N %/% K] vector[K] rows = to_vector_of_arrays(mu, K);
  if (abs(rows[2][1] - mu[3]) > 0) reject("row order");
  for (r in 1 : (N %/% K)) rows[r] ~ normal(0, 1);
  y ~ normal(mu, 1);
}
