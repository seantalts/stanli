functions {
  int integer_division(int a, int b) {
    int i = 0;
    while (b * (i + 1) <= a) {
      i = i + 1;
    }
    return i;
  }
  array[] vector to_vector_of_arrays(vector vec, int length_array) {
    array[integer_division(num_elements(vec), length_array)] vector[length_array] res;
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
  array[integer_division(N, K)] real w;
}
parameters {
  vector[N] mu;
}
model {
  array[N %/% K] vector[K] rows = to_vector_of_arrays(mu, K);
  w ~ normal(mu[1], 1);
  for (r in 1 : (N %/% K)) rows[r] ~ normal(r, 1);
  y ~ normal(mu, 1);
}
