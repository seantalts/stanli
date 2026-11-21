functions {
  real normal_density(real x,      // Function argument
                      real xc,     // Complement of function argument
                                   //  on the domain (defined later)
                      real mu,     // mean
                      real sigma) {  // standard deviation
    return 1 / (sqrt(2 * pi()) * sigma) * exp(-0.5 * ((x - mu) / sigma)^2);
  }
}

data {
  int N;
  array[N] real y;
}

parameters {
  real mu;
  real<lower=0.0> sigma;
  real left_limit;
}

model {
  mu ~ normal(0, 1);
  sigma ~ normal(0, 1);
  left_limit ~ normal(0, 1);
  target += normal_lpdf(y | mu, sigma);
  target += -log(integrate_1d_double_exponential(normal_density,
                                                 left_limit,
                                                 positive_infinity(),
                                                 mu, sigma));
}
