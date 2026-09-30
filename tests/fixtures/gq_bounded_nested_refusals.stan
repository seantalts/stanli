data { int mode; }
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real result = 0;
  if (mode == 0) {
    array[x > 0 ? 1 : 2, x > 0 ? 2 : 3] real a;
    a[1, 1] = x;
    result = a[1, 1];
  } else if (mode == 1) {
    array[x > 0 ? 1 : 2, 2] real b;
    b[1] = {x, x};
    result = sum(b[1]);
  } else if (mode == 2) {
    array[x > 0 ? 1 : 2, 2] real c;
    for (i in 1:size(c)) for (j in 1:2) c[i, j] = x;
    result = sum(c[1, 1:1]);
  } else if (mode == 3) {
    array[x > 0 ? 1 : 2, 2] int d;
    for (i in 1:size(d)) for (j in 1:2) d[i, j] = j;
    result = sum(d[1]);
  } else if (mode == 4) {
    array[x > 0 ? 1 : 2] vector[2] e;
    for (i in 1:size(e)) for (j in 1:2) e[i, j] = x;
    result = sum(e[1]);
  } else if (mode == 5) {
    array[x > 0 ? 1 : 2, 2] real f;
    for (i in 1:size(f)) for (j in 1:2) f[i, j] = normal_rng(x, 1);
    result = sum(f[1]);
  } else if (mode == 6) {
    array[x > 0 ? 1 : 2, 2] real g;
    for (i in 1:size(g)) for (j in 1:2) g[i, j] = exp(x);
    result = sum(g[1]);
  } else {
    array[2, x > 0 ? 1 : 2] real h;
    h[1, 1] = x;
    result = h[1, 1];
  }
}
