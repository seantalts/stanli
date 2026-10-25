// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 7, "y": [1, 2, 3, 4, 5, 6, 7]}
data {
  int N;
  vector[N] y;
}
parameters {
  vector[N] mu;
}
model {
  int k = 0;
  while (2 * (k + 1) <= N) k += 1;
  vector[k] z = mu[1 : k];
  if (num_elements(z) != 3) reject("counter extent");
  z ~ normal(0, 1);
  y ~ normal(mu, 1);
}
