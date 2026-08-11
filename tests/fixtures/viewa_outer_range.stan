parameters {
  array[5] real a;
}
model {
  array[3] real picked = a[2:4];
  target += picked[1] + 10 * picked[2] + 100 * picked[3];
}
