// v[2:] (Upfrom) on a declared 3-vector parameter. tests/test_lower.cpp
// checks that it lowers like slice_between_o0's v[2:3]. Both are unoptimized
// so that partial evaluation does not rewrite one into the other.
parameters {
  vector[3] v;
}
model {
  target += normal_lpdf(v[2:] | 0, 1);
}
