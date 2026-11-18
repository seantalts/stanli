functions {
  // side has no columns when L is 0 and layers is empty when K is 0.
  tuple(vector, matrix, array[] matrix, array[] matrix)
    stack(data array[] int pick, int K, int L, vector w) {
    int M = size(w);
    matrix[M, L] side;
    array[K] matrix[M, 2] layers;
    array[K ? K : 1] matrix[M, 2] out;
    if (K) {
      out[:, :, 1] = rep_array(w, K);
      out[:, :, 2] = rep_array(-w, K);
      for (k in 1:K) {
        layers[k] = k * rep_matrix(w, 2);
        out[k, pick] += layers[k, pick];
        out[k, pick, 2] += 0.5 * w[pick];
      }
    } else {
      out[1, :, 1] = w;
      out[1, :, 2] = square(w);
    }
    for (l in 1:L) {
      side[:, l] = l * exp(w);
    }
    return (w .* w, side, layers, out);
  }
}
data {
  int<lower=1> M;
  int<lower=1> K;
  int<lower=1> L;
  array[2] int<lower=1, upper=M> pick;
}
parameters {
  vector[M] w;
}
transformed parameters {
  tuple(vector[M], matrix[M, 0], array[0] matrix[M, 2], array[1] matrix[M, 2])
    flat = stack(pick, 0, 0, w);
  tuple(vector[M], matrix[M, L], array[K] matrix[M, 2], array[K] matrix[M, 2])
    deep = stack(pick, K, L, w);
  array[K] matrix[M, 2] deep_out = stack(pick, K, L, w).4;
}
model {
  w ~ normal(0, 1);
  to_vector(flat.4[1]) ~ normal(0, 2);
  to_vector(deep.2) ~ normal(0, 4);
  for (k in 1:K) {
    to_vector(deep.3[k]) ~ normal(0, 3);
    to_vector(deep_out[k]) ~ normal(0, 3);
  }
}
