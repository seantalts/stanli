parameters {
  array[2, 3] real a;
}
model {
  array[2] real picked = a[:, 2];
  target += 100 * size(picked) + 10 * picked[1] + picked[2];
}
