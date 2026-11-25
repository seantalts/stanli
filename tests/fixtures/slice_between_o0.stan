// v[2:3] (Between) on a declared 3-vector parameter; see slice_upfrom_o0.
parameters {
  vector[3] v;
}
model {
  target += normal_lpdf(v[2:3] | 0, 1);
}
