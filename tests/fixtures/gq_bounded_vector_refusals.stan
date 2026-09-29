data { int mode; }
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  if (mode == 0) {
    vector[x > 0 ? 1 : 2] a;
    a = rep_vector(x, 2);
    result = sum(a);
  } else if (mode == 1) {
    row_vector[x > 0 ? 1 : 2] a;
    for (i in 1:cols(a)) a[i] = x;
    result = sum(a[1:1]);
  } else if (mode == 2) {
    array[x > 0 ? 1 : 2] vector[2] a;
    for (i in 1:size(a)) a[i] = rep_vector(x, 2);
    result = sum(a[1]);
  } else {
    vector[x > 0 ? 1 : 2] a;
    for (i in 1:rows(a)) a[i] = normal_rng(x, 1);
    result = sum(a);
  }
  real after = normal_rng(0, 1);
}
