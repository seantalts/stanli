// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"pick": [2, 1]}
// Registered slice functions on rank-2 arrays inside a runtime-control
// region. The register file stores arrays outer-major (graph order), and
// the shared resolvers must be asked for that order: the first SliceView
// wiring passed first-index-fast maps here, which mis-selected cells of
// every rank>=2 scalar-leaf array (head(x, 1) read x[1,1], x[1,3], x[2,2]).
// Cross checks were blind -- both compared lp paths share the region
// Program -- so this fixture self-checks the values and rejects on any
// future mismatch, which lit reports as EVAL_FAIL.
data {
  array[2] int pick;
}
parameters {
  real probe;
}
model {
  probe ~ std_normal();
  if (probe > -1e100) {
    array[2, 3] real x;
    for (i in 1 : 2)
      for (j in 1 : 3) x[i, j] = probe * 0.125 + i * 10 + j;
    array[1, 3] real h = head(x, 1);
    array[2, 3] real rv = reverse(x);
    array[3, 3] real ap = append_array(x, {{91.5, 92.5, 93.5}});
    if (abs(h[1, 2] - (probe * 0.125 + 12)) > 1e-12) reject("head cell");
    if (abs(h[1, 3] - (probe * 0.125 + 13)) > 1e-12) reject("head cell 3");
    if (abs(rv[1, 1] - (probe * 0.125 + 21)) > 1e-12) reject("reverse cell");
    if (abs(rv[2, 3] - (probe * 0.125 + 13)) > 1e-12) reject("reverse cell 3");
    if (abs(ap[3, 2] - 92.5) > 1e-12) reject("append cell");
    if (abs(ap[2, 1] - (probe * 0.125 + 21)) > 1e-12) reject("append cell 1");
    if (abs(x[pick[1], pick[2]] - (probe * 0.125 + 21)) > 1e-12)
      reject("data-index cell");
    target += h[1, 2] + rv[1, 1] * 3 + ap[3, 3] * 5;
  }
}
