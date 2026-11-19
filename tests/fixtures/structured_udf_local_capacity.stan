functions {
  // A local sized by an argument, and one sized by a local computed from a
  // data argument.
  vector group_terms(int n, int first, data array[] int parts, vector x,
                     real scale) {
    int total = sum(parts);
    vector[n] v;
    vector[total] w;
    for (i in 1:n) v[i] = scale * x[first + i - 1];
    for (j in 1:total) w[j] = j;
    return v * (sum(w) / total);
  }
}
data {
  int<lower=1> G;
  int<lower=1> N;
  array[G] int<lower=1> size;
  array[G] int<lower=1> first;
  array[3] int<lower=1> parts;
  vector[N] y;
  vector[N] x;
}
parameters {
  vector[G] mu;
  real beta;
  real<lower=0> sigma;
}
model {
  for (g in 1:G)
    target += normal_lpdf(y[first[g]] | mu[g] + sum(group_terms(size[g],
                                                                first[g], parts,
                                                                x, beta)),
                          sigma);
  mu ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma ~ lognormal(0, 1);
}
generated quantities {
  real fit = 0;
  for (g in 1:G)
    fit += sum(group_terms(size[g], first[g], parts, x, beta));
}
