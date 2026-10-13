functions {
  vector pick(int n, real a) {
    if (n % 2 == 0) return rep_vector(a, 2);
    return rep_vector(a, 3);
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) target += -0.5 * dot_self(pick(n, y[n] - mu));
}
