// Truncation of a parameter vector with int bounds.
parameters {
  vector<lower=1, upper=5>[2] z;
  real mu;
  vector<lower=0>[2] w;
  real nu;
}
model {
  z ~ normal(mu, 1) T[1, 5];
  w ~ normal(nu, 1) T[0, ];
}
