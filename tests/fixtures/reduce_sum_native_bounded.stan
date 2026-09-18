functions {
  real partial(array[] real z, int first, int last, real b) {
    return sum(z) * b;
  }
}
data { int<lower=2> N; array[N] real y; }
parameters { real b; }
model {
  real acc = 0;
  int k = 1;
  while (k <= N) { acc += b; k += 1; }
  target += -0.5 * square(acc) + reduce_sum(partial, y, 1, b);
}
