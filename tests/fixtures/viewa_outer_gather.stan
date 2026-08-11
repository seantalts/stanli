data {
  array[3] int idx;
}
parameters {
  array[5] real a;
}
model {
  array[3] real picked = a[idx];
  target += picked[1] + 10 * picked[2] + 100 * picked[3];
}
