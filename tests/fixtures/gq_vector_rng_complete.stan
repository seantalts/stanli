parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  vector[3] probabilities = softmax([x, -0.5, 1.0]');
  matrix[3,3] L = [[1.0,0.0,0.0], [0.2,1.5,0.0], [-0.1,0.3,0.7]];
  real before = normal_rng(0, 1);
  int categorical = categorical_rng(probabilities);
  int categorical_logit = categorical_logit_rng([x, -0.5, 1.0]');
  int poisson_binomial = poisson_binomial_rng(probabilities);
  vector[3] multi_normal_cholesky = multi_normal_cholesky_rng(rep_vector(x, 3), L);
  int choices = 1;
  int counts = 0;
  vector[3] samples = rep_vector(0, 3);
  int i = 1;
  while (i <= 2) {
    choices = categorical_rng(probabilities);
    choices = categorical_logit_rng([x, -0.5, 1.0]');
    counts = poisson_binomial_rng(probabilities);
    samples = multi_normal_cholesky_rng(rep_vector(x, 3), L);
    i += 1;
  }
  real indexed = probabilities[choices];
  real after = normal_rng(0, 1);
}
