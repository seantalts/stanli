// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: dirichlet_lpdf: probabilities is not a valid simplex
// STANLI-LIT-DATA: {"theta": [0.2, 0.5, 0.6], "alpha": [1, 2, 3]}
// Under ~ every term of a data-only Dirichlet drops, but Stan Math still
// checks that theta is a simplex.
data {
  vector[3] theta;
  vector[3] alpha;
}
parameters { real mu; }
model {
  theta ~ dirichlet(alpha);
  mu ~ normal(0, 1);
}
