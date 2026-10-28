functions {
  array[] int truncate_all(data array[] real x) {
    return to_int(x);
  }
}
data {
  array[2, 2] real r;
}
transformed data {
  array[2, 2] int ri = to_int(r);
  int s = to_int(-2.7);
  int same = to_int(7);
  array[3] int u = truncate_all({2.9, -1.7, 5.5});
  array[2] int ii = to_int({4, -3});
}
parameters {
  real q;
}
transformed parameters {
  real tq = q * to_int(r[2, 1]);
}
model {
  array[2, 2] int ml = to_int(r);
  target += -0.5 * square(q);
  target += q * (ri[1, 2] + 10 * ri[2, 1] + 100 * s + 1000 * same
                 + 10000 * sum(u) + 100000 * ml[1, 1] + 1000000 * ii[2]);
}
generated quantities {
  array[2, 2] int gi = to_int(r);
  array[2, 2] int gl = to_int({{1.0, 2.0}, {3.0, 4.0}});
  int b = gl[2, 1];
}
