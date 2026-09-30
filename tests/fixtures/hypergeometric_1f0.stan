functions {
  real coverage(real a, real z) {
    return hypergeometric_1F0(a, z);
  }
}
data {
  int<lower=0> N;
  vector[N] offsets;
  real z_data;
}
transformed data {
  real prepared = coverage(0.5, z_data);
}
parameters {
  real a;
  real z_raw;
}
transformed parameters {
  real z = 0.2 * tanh(z_raw);
}
model {
  a ~ std_normal();
  z_raw ~ std_normal();
  target += prepared * a;
  target += coverage(a, z_data) + coverage(0.5, z);
  for (i in 1:N) {
    if (a > 0)
      target += coverage(a, z + offsets[i]);
    else
      target += coverage(-a, z - offsets[i]);
  }
}
generated quantities {
  real both_active = coverage(a, z);
  real first_active = coverage(a, z_data);
  real second_active = coverage(0.5, z);
  real from_data = prepared;
}
