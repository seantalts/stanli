// y-side of a GLM as a parameter: at --O1 stanc3 rewrites
// theta ~ normal(x * b, 1) into normal_id_glm_lupdf(theta | x, 0, b, 1),
// so the kernel must propagate the gradient to theta, not just to b.
data {
  int N;
  int K;
  matrix[N, K] x;
}
parameters {
  vector[N] theta;
  vector[K] b;
}
model {
  theta ~ normal(x * b, 1);
}
