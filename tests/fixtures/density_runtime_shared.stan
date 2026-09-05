data {
  int y;
  int N;
  array[2] int z;
  int c;
  array[2] int cats;
  matrix[2, 1] X;
}
parameters {
  real theta;
}
model {
  if (theta > 0) {
    vector[3] logits = [theta, 0, -theta]';
    target += beta_binomial_lpmf(y | N, exp(theta), 2.2);
    target += bernoulli_logit_glm_lpmf(z | X, theta, [0.3]');
    target += categorical_lpmf(c | softmax(logits));
    target += categorical_logit_lpmf(cats | logits);
    target += hypergeometric_lpmf(y | N, 4, 4);
    target += discrete_range_lpmf(y | 1, N);
  }
}
