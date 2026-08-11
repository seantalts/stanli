data {
  array[0] vector[0] empty;
}
parameters {
  real q;
}
model {
  array[2] vector[0] values = empty;
  target += q + size(values);
}
