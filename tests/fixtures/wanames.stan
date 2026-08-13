// Column-naming shapes for tests/test_write_array.cpp. One of each case the
// CSV naming rules must get right, chosen because two of them were real
// bugs found by diffing headers against CmdStan (verify_refs.py --wa-headers):
//   s     scalar          -> "s"        (bare, never "s.1")
//   v     vector[1]       -> "v.1"      (a container is indexed even at
//                                        length one; we wrote "v")
//   M     matrix[2,3]     -> "M.1.1", "M.2.1", "M.1.2", ...
//                                       (two indices, column-major; we wrote
//                                        "M.1" .. "M.6")
//   gq    array of vector -> "gq.1.1", "gq.1.2", "gq.2.1", ...
//                                       (stanc writes one element per
//                                        FnWriteParam; the array index joins
//                                        the name, outermost first)
parameters {
  real s;
  vector[1] v;
  matrix[2, 3] M;
}
model {
  s ~ normal(0, 1);
}
generated quantities {
  array[2] vector[2] gq;
  for (i in 1 : 2) {
    for (j in 1 : 2) {
      gq[i][j] = s + i * 10 + j;
    }
  }
}
