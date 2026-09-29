data { int mode; }
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  if (mode == 0) {
    array[x > 0 ? 1 : 2] real a;
    a = rep_array(x, 2);
    result = sum(a);
  } else if (mode == 1) {
    array[x > 0 ? 1 : 2] int a;
    for (i in 1:size(a)) a[i] = i;
    result = sum(a);
  } else if (mode == 2) {
    array[x > 0 ? 1 : 2] real a;
    for (i in 1:size(a)) a[i] = normal_rng(x, 1);
    result = sum(a);
  } else if (mode == 3) {
    array[x > 0 ? 1 : 2] real a;
    for (i in 1:size(a)) a[i] = x;
    result = dims(a)[1];
  } else {
    array[x > 0 ? 1 : 2] real a;
    int i = 1;
    while (i <= size(a)) {
      a[i] = x;
      i += 1;
    }
    result = sum(a);
  }
  real after = normal_rng(0, 1);
}
