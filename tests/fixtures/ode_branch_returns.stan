functions {
  vector exit_rhs(real t, vector y, real rate) {
    for (k in 1:3) {
      if (rate > 0.05) return -rate * y;
    }
    while (rate < -0.01) {
      if (t >= 0) return rate * y;
      return -y;
    }
    return -0.1 * y;
  }
}
parameters { real rate; }
transformed parameters {
  real prediction = ode_rk45(exit_rhs, [1.0]', 0.0, {0.4}, rate)[1,1];
}
model {
  rate ~ normal(0, 1);
  prediction ~ normal(0.5, 0.3);
}
generated quantities { real squared_prediction = square(prediction); }
