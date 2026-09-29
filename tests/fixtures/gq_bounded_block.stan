data {
  int mode;
  int lower_size;
  int upper_size;
  int repeats;
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  {
    int length = x > 0 ? lower_size : upper_size;
    array[length] real temporary;
    length = 0;
    for (rep in 1:repeats) {
      for (i in 1:size(temporary)) {
        if (mode != 3 || i == 1) temporary[i] = x + i;
      }
    }
    if (mode == 1) result = temporary[2];
    else if (mode == 2) {
      temporary[2] = x;
      result = sum(temporary);
    } else if (mode == 4) {
      int index = x > 0 ? 2 : 1;
      result = temporary[index];
    } else result = sum(temporary) + size(temporary) + num_elements(temporary);
  }
  real after = normal_rng(0, 1);
}
