// RNG calls in transformed data. CmdStan draws these once, at model
// construction, from create_rng(seed, 0); stanli draws them once at compile
// time from the same stream, through the interpreted RNG handler that
// write_array's interpreter uses. The families span the interpreter-only
// tranche (poisson, gamma), the graph-native scalars (normal), the container
// draws (dirichlet, multi_normal), an elementwise-broadcast call, and a user
// `_rng` function, so one fixture covers every shape the handler serves.
// Generated quantities copy every draw out so a test can read them.
functions {
  real shifted_rng(real m) {
    return normal_rng(m, 1);
  }
}
transformed data {
  real z = normal_rng(0, 1);
  int k = poisson_rng(3.0);
  vector[3] d = dirichlet_rng([1, 2, 3]');
  vector[2] mv = multi_normal_rng([0, 0]', [[1, 0.2], [0.2, 1]]);
  array[2] real g = gamma_rng({2.0, 3.0}, 1.5);
  real u = shifted_rng(z);
}
parameters {
  real mu;
}
model {
  mu ~ normal(z, 1);
}
generated quantities {
  real td_z = z;
  int td_k = k;
  vector[3] td_d = d;
  vector[2] td_mv = mv;
  array[2] real td_g = g;
  real td_u = u;
}
