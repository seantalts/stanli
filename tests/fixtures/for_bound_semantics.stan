functions {
  real shrink(real x) {
    int last = 4;
    real result = 0;
    for (i in 1:last) {
      result += x * i;
      last = 1;
    }
    return result;
  }
  real grow(real x) {
    int last = 1;
    real result = 0;
    for (i in 1:last) {
      last = 3;
      if (i == 2) continue;
      result += x * i;
    }
    return result;
  }
  int bound_rng(int n) {
    real ignored = uniform_rng(0, 1);
    return ignored > 0 ? n : 0;
  }
  real bound_edges(real x) {
    int last = 3;
    real result = 0;
    array[3] real lookup = {x, 2 * x, 3 * x};
    for (i in 1:last) {
      if (x > 0) {
        last = 2;
      } else {
        last = 1;
      }
      result += x;
    }
    result += lookup[last];
    for (i in 1:last) result += x;
    last = 3;
    for (i in 1:last) {
      real again = 1;
      while (again > 0) {
        last = 1;
        again = 0;
      }
      result += x;
    }
    return result;
  }
  vector rhs(real t, vector y, real rate) {
    int last = y[1] > 0 ? 4 : 0;
    real result = 0;
    for (i in 1:last) {
      result += rate * y[1];
      last = 1;
    }
    return [-result]';
  }
}
data { array[2] real times; }
parameters { real rate; }
transformed parameters {
  real a = shrink(rate);
  real b = grow(rate);
  real c = bound_edges(rate);
  array[2] vector[1] solution = ode_rk45(rhs, [1.0]', 0, times, rate);
}
model {
  rate ~ normal(0, 1);
  target += a + b + c;
  solution[2] ~ normal(0, 1);
}
generated quantities {
  real draw;
  real visits = 0;
  int last = 1;
  for (i in 1:last) {
    visits += 1;
    last = 3;
  }
  for (i in bound_rng(1):bound_rng(3)) visits += 1;
  for (i in bound_rng(5):bound_rng(3)) visits += 1;
  draw = uniform_rng(0, 1);
}
