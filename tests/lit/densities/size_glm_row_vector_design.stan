// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"y": [1, 2, 3], "x": [1, 2]}
// With a row_vector design, normal_id_glm has one instance per outcome, so a
// per-outcome intercept and scale are valid.
data { vector[3] y; row_vector[2] x; }
parameters { vector[3] alpha; vector[2] beta; vector<lower=0>[3] sigma; }
model {
  y ~ normal_id_glm(x, alpha, beta, sigma);
  alpha ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma ~ lognormal(0, 1);
}
