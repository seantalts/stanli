
functions {
  real mo(vector scale, int i) {
    if (i == 0) {
      return 0;
    } else {
      return rows(scale) * sum(scale[1:i]);
    }
  }
}
data {
  int<lower=1> N; vector[N] Y; int<lower=1> Imo;
  array[N] int Xmo_1; int<lower=1> Jmo_1; int prior_only;
}
parameters {
  real Intercept; real<lower=0> sigma; real bsp_1;
  simplex[Jmo_1] simo_1;
}
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += dirichlet_lpdf(simo_1 | rep_vector(1.0, Jmo_1));
}
model {
  if (!prior_only) {
    vector[N] mu = rep_vector(0.0, N);
    for (n in 1:N) {
      mu[n] += Intercept + bsp_1 * mo(simo_1, Xmo_1[n]);
    }
    target += normal_lpdf(Y | mu, sigma);
  }
  target += lprior;
}
