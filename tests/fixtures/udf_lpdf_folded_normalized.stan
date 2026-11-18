functions {
  real shifted_lpdf(real y) {
    if (0.5 > 1.0) {
      return 0.5;
    }
    return normal_lupdf(y | 0.5, 0.5);
  }
  real fixed_lpdf(real y) {
    return normal_lupdf(0.25 | 0.5, 0.5);
  }
}
parameters {
  real p;
}
transformed parameters {
  real t = shifted_lpdf(0.25 | );
  real u = 0;
  if (p < 100) {
    u = fixed_lpdf(p | );
  }
}
model {
  target += t;
  target += u;
  target += shifted_lpdf(p | );
}
generated quantities {
  real g = shifted_lpdf(0.75 | );
}
