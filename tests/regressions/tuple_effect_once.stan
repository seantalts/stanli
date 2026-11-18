functions {
  real pair_sum(tuple(real, real) t) {
    return t.1 + t.2;
  }

  tuple(real, real) draw_pair_rng(real mu) {
    print("draw_pair_rng ", mu);
    return (normal_rng(mu, 1), 1.0);
  }

  tuple(real, real) draw_pair_rng(vector mu) {
    return (normal_rng(mu[1], 1), 2.0);
  }
}
transformed data {
  real td = pair_sum(draw_pair_rng(0.5));
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  real y = pair_sum(draw_pair_rng(mu));
  real z = normal_rng(0, 1);
  real td_out = td;
}
