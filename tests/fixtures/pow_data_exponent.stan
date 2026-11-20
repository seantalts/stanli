data {
  array[4] real e;
}
parameters {
  vector[5] x;
  real t;
}
model {
  target += sum(pow(x, 2)) + sum(pow(x, 0.5)) + sum(pow(x, -1))
            + sum(pow(x, -2.0)) + sum(pow(x, -0.5)) + sum(pow(x, 1));
  for (i in 1:4) target += pow(t, e[i]);
}
