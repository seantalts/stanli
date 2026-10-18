// The posterior mode is at mu = -1, where generated quantities reject.
parameters {
  real mu;
}
model {
  mu ~ normal(-1, 1);
}
generated quantities {
  real y = 0;
  if (mu < 0) reject("mu is negative");
}
