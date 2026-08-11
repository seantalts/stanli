parameters {
  array[2, 3] real a;
}
model {
  array[2, 2] real picked = a[1:2, {1, 3}];
  target += picked[1, 1];
}
