functions {
  real shifted_rng(real m, real s) {
    real u = normal_rng(m, s);
    if (u > m) {
      return u + exponential_rng(1);
    }
    return u;
  }
}
data {
  int<lower=1> N;
  int<lower=1> K;
  vector[N] x;
  array[N] int<lower=1, upper=K> len;
  int<lower=0> T;
}
parameters {
  real mu;
  real ls;
}
model {
  mu ~ normal(0, 1);
  ls ~ normal(0, 1);
}
generated quantities {
  array[T] vector[3] pad;
  for (t in 1:T) {
    pad[t] = softmax([mu * t, 1, mu + t]');
  }
  real sigma = exp(ls);

  vector[N] a;
  real acc = 0;
  for (n in 1:N) {
    real unused = normal_rng(0, 1);
    a[n] = normal_rng(mu * x[n], sigma);
    acc += a[n] + normal_rng(0, 1);
  }

  array[N] int hits;
  real tl = 0;
  for (n in 1:N) {
    real d = normal_rng(mu, 1);
    if (d > 0) {
      hits[n] = poisson_log_rng(mu + x[n]);
      tl += uniform_rng(0, 1);
    } else {
      hits[n] = 0;
    }
    tl += d;
  }

  array[N, K] int counts;
  int total = 0;
  for (n in 1:N) {
    array[K] int c = poisson_log_rng(mu * x[n] + rep_vector(0.1, K));
    counts[n] = c;
    total += c[1] + c[K];
  }

  int ragged = 0;
  for (n in 1:N) {
    array[len[n]] int r = poisson_log_rng(rep_vector(mu, len[n]));
    ragged += r[len[n]];
  }

  vector[N] u;
  real usum = 0;
  for (n in 1:N) {
    u[n] = shifted_rng(mu * x[n], sigma);
    usum += u[n];
  }

  array[N, K] int z;
  int zsum = 0;
  for (n in 1:N) {
    for (k in 1:K) {
      z[n, k] = bernoulli_logit_rng(mu * x[n] - k);
      zsum += z[n, k];
    }
  }
}
