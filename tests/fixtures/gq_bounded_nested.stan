data {
  int mode;
  int lower_size;
  int upper_size;
  int width;
  int repeats;
}
parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real result = 0;
  {
    int length = x > 0 ? lower_size : upper_size;
    array[length, width] real temporary;
    length = 0;
    for (rep in 1:repeats) {
      for (i in 1:size(temporary)) {
        for (j in 1:width) {
          if (mode != 3 || j == 1)
            temporary[i, j] = mode == 5
              ? (j == 1 ? 1e16 : (j == 3 ? -1e16 : 1)) : x + i + j;
        }
      }
    }
    if (mode == 1) result = temporary[2, 1];
    else if (mode == 2) temporary[2, 1] = x;
    else if (mode == 4) {
      int index = x > 0 ? 2 : 1;
      result = temporary[index, index];
    } else if (mode == 6) result = sum(temporary[2]);
    for (r in 1:size(temporary)) result += sum(temporary[r]);
    result += size(temporary) + num_elements(temporary);
    if (mode == 7)
      for (k in 1:num_elements(temporary)) result += x;
  }
  real after = normal_rng(0, 1);
}
