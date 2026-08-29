functions {
  array[] int matching_indices(array[] int x, int test) {
    array[size(x)] int hit;
    for (i in 1:size(x)) hit[i] = x[i] == test;
    array[sum(hit)] int out;
    int at = 1;
    for (i in 1:size(x)) {
      if (hit[i]) {
        out[at] = i;
        at += 1;
      }
    }
    return out;
  }
}
data {
  int<lower=0> N;
  array[N] int x;
}
parameters {
  real theta;
}
model {
  int done = 0;
  while (theta > done) {
    array[size(matching_indices(x, 1))] int selected
        = matching_indices(x, 1);
    target += (size(selected) + sum(selected)) * theta;
    done += 1;
  }
  target += theta;
}
generated quantities {
  array[size(matching_indices(x, 1))] int selected
      = matching_indices(x, 1);
  int selected_count = size(selected);
  int selected_sum = sum(selected);
}
