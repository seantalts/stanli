data {
  int mode;
  int lower_size;
  int upper_size;
  int repeats;
  int shift;
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  {
    int length = x > 0 ? lower_size : upper_size;
    array[length] int temporary;
    length = 0;
    if (mode == 2) {
      if (x > 0) {
        for (positive in 1:size(temporary)) temporary[positive] = positive + shift;
      } else {
        for (j in 1:size(temporary)) temporary[j] = shift - j;
      }
    } else {
      for (i in 1:size(temporary)) temporary[i] = i + shift;
    }
    if (mode == 1) result = temporary[2];
    else if (mode == 3) {
      array[sum(temporary)] real dependent;
      for (output_index in 1:size(dependent)) dependent[output_index] = x;
      result = sum(dependent);
    } else if (mode == 4) result = sum(temporary);
    else result = sum(temporary) + size(temporary);
  }
  real after = normal_rng(0, 1);
}
