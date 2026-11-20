data {
  array[7] int<lower=0> history;
}
parameters {
  array[2] real<lower=0, upper=1> phi;
  array[3] real<lower=0, upper=1> p;
}
transformed parameters {
  array[2] real<lower=0, upper=1> chi;
  chi[2] = (1 - phi[2]) + phi[2] * (1 - p[3]);
  chi[1] = (1 - phi[1]) + phi[1] * (1 - p[2]) * chi[2];
}
model {
  target += history[2] * log(chi[2]);
  target += history[3] * (log(phi[2]) + log(p[3]));
  target += history[4] * (log(chi[1]));
  target += history[5] * (log(phi[1]) + log1m(p[2])
                            + log(phi[2]) + log(p[3]));
  target += history[6] * (log(phi[1]) + log(p[2])
                            + log(chi[2]));
  target += history[7] * (log(phi[1]) + log(p[2])
                            + log(phi[2]) + log(p[3]));
}
generated quantities {
  real<lower=0, upper=1> beta3;
  beta3 = phi[2] * p[3];
}
