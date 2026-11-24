// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"tp": {"1": 1.5, "2": 3}, "atp": [{"1": 2.5, "2": 4}]}
data {
  tuple(real, array[1] real) tp;
  array[1] tuple(real, array[1] real) atp;
}
transformed data {
  if (tp.1 != 1.5 || size(tp.2) != 1 || tp.2[1] != 3) reject("tp is not (1.5, [3])");
  if (atp[1].1 != 2.5 || size(atp[1].2) != 1 || atp[1].2[1] != 4) reject("atp is not [(2.5, [4])]");
}
parameters { real mu; }
model { mu ~ normal(0, 1); }
