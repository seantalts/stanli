// reject() inside the model block below a data-set cutoff: 0 makes the
// sampler reject about half its proposals, a very low cutoff makes it never.
data { real y; real cutoff; }
parameters { real mu; }
model {
  if (mu < cutoff) reject("mu must be >= ", cutoff, ", found mu = ", mu);
  mu ~ normal(0, 1);
  y ~ normal(mu, 1);
}
