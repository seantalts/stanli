data { int<lower=0> N; }
parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  array[N] int draws;
  vector[N] predictive;
  real score = 0;
  vector[3] lookup = [1.25, 2.5, 4.0]';
  for (n in 1:N) {
    int w;
    w = poisson_rng(exp(x));
    int attempts = 0;
    while (w == 0 && attempts < 20) {
      w = poisson_rng(exp(x));
      attempts += 1;
    }
    draws[n] = w;
    if (bernoulli_rng(inv_logit(x))) {
      int k = 1 + binomial_rng(2, 0.5);
      score += lookup[k];
    }
    predictive[n] = student_t_rng(5, x, 1.25);
  }
}
