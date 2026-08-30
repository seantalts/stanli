data {
  matrix[2, 2] x;
  vector[2] y;
}
parameters {
  real alpha;
  vector[2] beta;
  real<lower=0> sigma;
}
model {
  int i = 0;
  matrix[2, 2] z = x;
  while (i < 1) {
    z = 2 * z;
    i += 1;
  }
  y ~ normal_id_glm(z, alpha, beta, sigma);
}
