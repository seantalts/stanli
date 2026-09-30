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
      for (i in 1:size(a)) if (x > 0) a[i] = i;
    } else if (mode == 1) {
      for (j in 1:size(a)) {
        if (x > 0) a[j] = j;
        else a[1] = 1;
      }
    } else if (mode == 2) {
      for (k in 1:size(a)) {
        a[k] = 1;
        a[1] = 1073741824;
      }
    } else if (mode == 3) {
      for (p in 1:size(a)) {
        a[p] = 1;
        if (x > 0) a[p] = -1073741825;
      }
    } else if (mode == 4) {
      for (q in 1:size(a)) {
        if (x > 0) a[q] = carried;
        else a[q] = 1;
        carried = 1;
      }
    } else if (mode == 5) {
      for (r in 1:size(a)) {
        a[r] = 1;
        a[r] = a[r] + 1;
      }
    } else if (mode == 6) {
      for (s in 1:size(a)) {
        int local_value;
        if (x > 0) local_value = s;
        else local_value = -s;
        a[s] = local_value;
      }
    } else if (mode == 7) {
      for (t in 1:size(a)) {
        for (inner in 1:(x > 0 ? 1 : 0)) a[t] = 1;
      }
    } else if (mode == 8) {
      for (u in 1:size(a)) a[u] = u;
      a[1] = 100;
    } else {
      for (v in 1:size(a)) {
        if (x > 0) a[v] = 1073741824;
        else a[v] = -1073741825;
      }
    }
    result = sum(a);
  }
  real after = normal_rng(0, 1);
}
