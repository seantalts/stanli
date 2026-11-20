// Functions that return a container from more than one place, with
// conditions on a parameter and on data. The overloads keep stanc from
// inlining them, so stanli rewrites the returns itself.
functions {
  vector guard(vector x) {
    if (x[1] > 0) return x;
    return -x;
  }
  real guard(real x) {
    return x;
  }
  matrix chain(matrix m, real c) {
    if (c > 1) return 2 * m;
    else if (c > -1) return m';
    else return -m;
  }
  real chain(real x) {
    return x;
  }
  row_vector first_below(row_vector v, real bound) {
    for (i in 1:cols(v)) {
      for (j in 1:2) {
        if (v[i] * j < bound) return v * j;
      }
    }
    return v;
  }
  real first_below(real x) {
    return x;
  }
  array[] real pick(real c) {
    if (c < 0) return {c, 1.0};
    return {2.0, c};
  }
  real pick(vector x) {
    return 0;
  }
  array[] int counts(real c) {
    if (c < 0) return {1, 2};
    return {3, 4};
  }
  real counts(vector x) {
    return 0;
  }
}
parameters {
  real mu;
}
model {
  matrix[2, 2] m = [[mu, 1], [2, 3 * mu]];
  target += sum(guard([mu, 2]')) + 0.5 * sum(guard([-mu, 2]'));
  for (c in {-2, 0, 2})
    target += c * sum(chain(m, c)[1]);
  target += sum(chain(m, 30 * mu)[1]) + 2 * sum(chain(m, -30 * mu)[1])
            + 3 * sum(chain(m, 10 * mu)[1]);
  for (k in 1:3)
    target += k * sum(first_below([0.1, mu * k], -0.05));
  target += sum(first_below([0.1, mu], 2 * mu));
  target += sum(pick(mu)) + 0.5 * sum(pick(-mu));
  target += (sum(counts(mu)) + 0.5 * sum(counts(-mu))) * mu;
  mu ~ normal(0, 1);
}
