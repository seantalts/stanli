// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "yi": [1, 0, 1], "y": [1, 2, 3], "X": [[1, 2], [3, 4], [5, 6]], "xr": [0.5, 0.25]}
// An intercept and a scale with one value per row are valid, with a matrix
// design and, for the integer GLMs, a row_vector design.
data {
  int N;
  array[N] int yi;
  vector[N] y;
  matrix[N, 2] X;
  row_vector[2] xr;
}
parameters { vector[N] a; vector[2] b; vector<lower=0>[N] s; }
model {
  yi ~ poisson_log_glm(X, a, b);
  yi ~ poisson_log_glm(xr, a, b);
  yi ~ bernoulli_logit_glm(X, a, b);
  y ~ normal_id_glm(X, a, b, s);
  a ~ normal(0, 1);
  b ~ normal(0, 1);
  s ~ lognormal(0, 1);
}
