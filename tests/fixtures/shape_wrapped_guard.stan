// Shape-only conditions through value wrappers.  None of these predicates
// reads a parameter value: transpose changes matrix orientation, indexing an
// array drops its outer dimension, and dims materializes declared geometry.
data {
  int<lower=0> n;
}
parameters {
  matrix[n, 2] m;
  array[2] vector[n] a;
  real theta;
}
model {
  real scale = 0.0;
  if (rows(transpose(m)) == 2)
    scale += 1.0;
  else
    scale += 10.0;
  if (size(a[1]) == n)
    scale += 2.0;
  else
    scale += 20.0;
  if (dims(m)[1] == n)
    scale += 4.0;
  else
    scale += 40.0;
  target += scale * theta;
}
