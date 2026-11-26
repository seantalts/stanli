data { int<lower=1> N; array[N] real y; }
parameters { simplex[2] t1; simplex[2] t2; positive_ordered[2] mu; }
model {
  matrix[2, 2] Gamma = [t1', t2'];
  matrix[2, N] log_omega;
  for (k in 1:2) for (t in 1:N) log_omega[k, t] = normal_lpdf(y[t] | mu[k], 1);
  target += hmm_marginal(log_omega, Gamma, rep_vector(0.5, 2)) + log(2);
}
