// Indexed stores through the shared index geometry: mixed range/gather,
// upfrom, and one-index matrix-row writes that the named store paths used
// to refuse. Every write is followed by cell-level reject self-checks
// because value regressions here are invisible to CROSS (both compared lp
// paths share a lowering) and to print-based probes (the observation
// splice bug below corrupted only the compile-time fold, not the slots).
parameters {
  real theta;
}
model {
  // Deep-array gather write selects whole outer rows.
  array[3, 4] real a = rep_array(0.0, 3, 4);
  a[{1, 3}] = rep_array(1.0, 2, 4);
  if (abs(a[1, 2] - 1) > 0 || abs(a[2, 2]) > 0 || abs(a[3, 4] - 1) > 0)
    reject("deep gather write picked the wrong rows");
  // Mixed range/gather write over both axes.
  array[3, 4] real b = rep_array(0.0, 3, 4);
  b[2 : 3, {1, 4}] = rep_array(2.0, 2, 2);
  if (abs(b[2, 1] - 2) > 0 || abs(b[2, 2]) > 0 || abs(b[3, 4] - 2) > 0
      || abs(b[1, 1]) > 0)
    reject("mixed range/gather write picked the wrong cells");
  // Upfrom write over the outer axis. c[4, 1] is the one probe cell where
  // outer-major and first-index-fast flat orders disagree: it regresses if
  // any store or fold path mixes the two, as the observation splice in
  // propagate_int_update once did for rank-2 bases.
  array[4, 2] real c = rep_array(0.0, 4, 2);
  c[3 :] = rep_array(3.0, 2, 2);
  if (abs(c[2, 1]) > 0 || abs(c[3, 2] - 3) > 0 || abs(c[4, 1] - 3) > 0)
    reject("upfrom write picked the wrong rows");
  // The same disagreement through the single-element store: this pins the
  // splice ordering fix on the path that existed before this phase.
  array[4, 2] real c2 = rep_array(0.0, 4, 2);
  c2[3, 1] = 3.0;
  if (abs(c2[3, 1] - 3) > 0 || abs(c2[1, 2]) > 0)
    reject("single write folded under the wrong order");
  // One-index matrix forms: upfrom and gather row writes.
  matrix[3, 2] m = rep_matrix(0.0, 3, 2);
  m[2 :] = rep_matrix(4.0, 2, 2);
  if (abs(m[1, 1]) > 0 || abs(m[2, 2] - 4) > 0 || abs(m[3, 1] - 4) > 0)
    reject("matrix upfrom row write picked the wrong rows");
  matrix[3, 2] m2 = rep_matrix(0.0, 3, 2);
  m2[{3, 1}] = rep_matrix(5.0, 2, 2);
  if (abs(m2[2, 1]) > 0 || abs(m2[1, 2] - 5) > 0 || abs(m2[3, 1] - 5) > 0)
    reject("matrix gather row write picked the wrong rows");
  // Matrix segment writes spelled with upfrom.
  matrix[3, 3] m3 = rep_matrix(0.0, 3, 3);
  m3[2 :, 2] = rep_vector(6.0, 2);
  m3[1, 2 :] = rep_row_vector(7.0, 2);
  if (abs(m3[2, 2] - 6) > 0 || abs(m3[3, 2] - 6) > 0 || abs(m3[1, 3] - 7) > 0
      || abs(m3[1, 1]) > 0)
    reject("matrix upfrom segment writes picked the wrong cells");
  target += theta ^ 2 + a[1, 1] + b[2, 1] + c[3, 1] + c2[3, 1] + m[2, 1]
            + m2[1, 1] + m3[2, 2];
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
