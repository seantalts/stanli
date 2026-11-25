// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 40, "y": [0.0, 0.81, -0.624, 0.0, -0.98, 0.425, 0.0, 1.131, -0.218, 0.0, -1.259, 0.007, 0.0, 1.361, 0.205, 0.0, -1.436, -0.413, 0.0, 1.483, 0.612, 0.0, -1.5, -0.799, 0.0, 1.487, 0.97, 0.0, -1.444, -1.122, 0.0, 1.372, 1.251, 0.0, -1.273, -1.356, 0.0, 1.148, 1.433, 0.0]}
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: ops=6
// STANLI-LIT-CHECK-NOT: LOOP
// A call that branches on a real argument bound to data unrolls like its
// inlined body in data_argument_branch_inline.stan; keep their checks
// identical. The overload keeps stanc from inlining hurdle_term.
functions {
  real hurdle_term(real y, real mu) {
    real lp;
    if (y == 0)
      lp = log(0.3);
    else
      lp = log(0.7) + normal_lpdf(y | mu, 1);
    return lp;
  }
  real hurdle_term(real y) {
    return y;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) target += hurdle_term(y[n], mu);
}
