data {
  int mode;
  int lower_size;
  int upper_size;
  int shift;
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  {
    int length = x > 0 ? lower_size : upper_size;
    array[length] int a;
    length = 0;
    if (mode == 0) {
      for (i in 1:size(a)) {
        if (x > 0) a[i] = i + shift;
        else a[i] = shift - i;
      }
    } else if (mode == 1) {
      for (j in 1:num_elements(a)) {
        a[j] = j;
        a[1] = shift;
      }
    } else if (mode == 2) {
      for (k in 1:size(a)) {
        if (x > 0) {
          if (k > 1) a[k] = shift;
          else a[k] = k;
        } else a[k] = shift - k;
      }
    } else if (mode == 3) {
      for (p in 1:size(a)) {
        a[p] = p;
        if (x > 0) a[p] = shift;
      }
    } else if (mode == 4) {
      for (q in 1:size(a)) {
        if (x > 0) a[q] = q;
        a[q] = shift - q;
      }
    } else if (mode == 6) {
      for (sentinel_index in 1:size(a)) {
        if (a[1] < 0) a[sentinel_index] = sentinel_index;
        else a[sentinel_index] = shift;
      }
    } else if (mode == 7) {
      for (outer in 1:size(a)) {
        a[outer] = outer;
        for (extra in 1:2) a[1] = shift;
      }
    } else if (mode == 8) {
      array[1] int tiny;
      for (tiny_index in 1:size(tiny)) {
        if (x > 0) tiny[tiny_index] = 1;
        else tiny[tiny_index] = 2;
      }
      for (destination in 1:size(a)) a[destination] = sum(tiny);
    } else {
      for (last in 1:size(a)) {
        if (x > 0) a[last] = last;
        else a[last] = shift;
      }
    }
    if (mode == 5) result = a[2];
    else result = sum(a);
  }
  real after = normal_rng(0, 1);
}
