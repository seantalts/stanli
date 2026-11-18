// stanc3's integration model user-distro-truncate.stan when form is 0, and
// one of its truncated statements alone otherwise.
functions {
  real foo_lpdf(real y, real mu) {
    return -(y - mu) ^ 2;
  }
  real foo_lcdf(real y, real mu) {
    return -1.7;
  }
  real foo_lccdf(real y, real mu) {
    return -0.02;
  }
}
data {
  int form;
}
parameters {
  real<lower=1, upper=5> y;
  real mu;
}
model {
  if (form == 0) {
    y ~ foo(mu);
    y ~ foo(mu) T[1, ];
    y ~ foo(mu) T[, 5];
    y ~ foo(mu) T[1, 5];
  } else if (form == 1) {
    y ~ foo(mu) T[1, ];
  } else if (form == 2) {
    y ~ foo(mu) T[, 5];
  } else {
    y ~ foo(mu) T[1, 5];
  }
}
