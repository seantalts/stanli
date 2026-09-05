// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"context_seed": 0.0, "pick": [2, 1]}
// Mixed Cartesian selections the graph used to refuse as unsupported
// index expressions, now resolved through the shared index geometry in
// every backend. Each read self-checks and rejects on a wrong cell, so
// a future storage-order or map regression reports as EVAL_FAIL.
functions {
  real idx_probe(real seed, array[] int pick) {
    real total = 0;
    array[2, 3] vector[4] x;
    for (i in 1 : 2)
      for (j in 1 : 3)
        for (k in 1 : 4) x[i, j] [k] = seed * 0.0625 + i * 100 + j * 10 + k;
    // Mixed selections over a deep array: ranges across array axes and
    // into the vector leaf, gathers, and upfrom -- the forms the graph
    // used to refuse as unsupported index expressions.
    array[2, 2] vector[4] a = x[1 : 2, 2 : 3];
    if (abs(a[2, 1][3] - (seed * 0.0625 + 223)) > 1e-12) reject("deep range");
    array[2] real b = x[1 : 2, 2, 1];
    if (abs(b[2] - (seed * 0.0625 + 221)) > 1e-12) reject("axis pick");
    vector[3] c = x[2, 3][2 : 4];
    if (abs(c[1] - (seed * 0.0625 + 232)) > 1e-12) reject("leaf range");
    array[2] vector[4] d = x[{2, 1}, pick[1]];
    if (abs(d[1][4] - (seed * 0.0625 + 224)) > 1e-12) reject("gather axis");
    vector[2] e = x[1, 1][3 :];
    if (abs(e[2] - (seed * 0.0625 + 114)) > 1e-12) reject("upfrom");
    array[2] matrix[2, 3] m;
    for (i in 1 : 2)
      for (r in 1 : 2)
        for (co in 1 : 3) m[i][r, co] = seed * 0.0625 + i + r * 10 + co * 100;
    matrix[2, 2] mm = m[2, 1 : 2, 2 : 3];
    if (abs(mm[1, 2] - (seed * 0.0625 + 2 + 10 + 300)) > 1e-12)
      reject("matrix leaf block");
    array[2] real me = m[1 : 2, 2, 3];
    if (abs(me[1] - (seed * 0.0625 + 1 + 20 + 300)) > 1e-12)
      reject("matrix leaf cell");
    total += a[1, 2][4] + b[1] * 3 + c[3] * 5 + d[2][1] * 7 + e[1] * 11
             + mm[2, 1] * 13 + me[2] * 17;
    return total;
  }
}
data {
  real context_seed;
  array[2] int pick;
}
transformed data {
  real transformed_data_result = idx_probe(context_seed, pick);
}
parameters {
  real probe;
}
model {
  probe ~ std_normal();
  target += idx_probe(probe, pick);
  if (probe > -1e100)
    target += idx_probe(probe + 0.25, pick);
}
generated quantities {
  real generated_quantities_result = idx_probe(probe, pick);
}
