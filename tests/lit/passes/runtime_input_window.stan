// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DUMP: log_prob:reduce
// STANLI-LIT-CHECK: live-ins: slot{{[0-9]+}}->r{{[0-9]+}}[len 5]
parameters { vector[8] x; }
model {
  real selected;
  if (x[3] > 0) selected = x[5];
  else selected = x[7];
  target += selected;
  x ~ std_normal();
}
