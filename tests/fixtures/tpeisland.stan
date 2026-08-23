// `target += <container>` inside a parameter-dependent branch, which
// lowering compiles into an island region rather than into graph ops.
transformed data {
  real thresh = 0.0;
}
parameters {
  vector[3] v;
  real z;
}
model {
  if (z > thresh) {
    target += v;
  } else {
    target += -v;
  }
}
generated quantities {
  real s = sum(v) + thresh;
}
