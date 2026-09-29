data { int mode; }
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  {
    array[x > 0 ? 2 : 3] int a;
    int carried = 100;
    if (mode == 0) {
      for (i1 in 1:(size(a) - 1)) a[i1] = i1;
    } else if (mode == 1) {
      for (i2 in 1:size(a)) {
        a[i2] = i2;
        a[1] = 100;
      }
    } else if (mode == 2) {
      for (i3 in 1:size(a)) {
        a[i3] = carried;
        carried = 1;
      }
    } else if (mode == 3) {
      for (i4 in 1:size(a)) if (x > 0) a[i4] = i4;
    } else if (mode == 4) {
      for (i5 in 1:size(a)) a[i5] = 1073741824;
    } else if (mode == 5) {
      for (i6 in 1:size(a)) a[i6] = i6 <= 2 ? 1073741824 : -1073741824;
    } else if (mode == 6) {
      for (i7 in 1:size(a)) a[i7] = 1;
      for (j in 1:size(a)) {
        result += sum(a);
        a[j] = 100;
      }
    } else if (mode == 7) {
      for (i8 in 1:size(a)) a[i8] = a[1];
    } else if (mode == 8) {
      if (x > 0) for (i9 in 1:size(a)) a[i9] = i9;
    } else {
      int n = size(a);
      for (i10 in 1:n) a[n - i10 + 1] = i10;
    }
    result += sum(a);
  }
  real after = normal_rng(0, 1);
}
