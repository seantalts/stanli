// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N0": 2}
// As break_flags_through_nested_loops, where no break runs. CmdStan ends
// with flag 0.
data {
  int N0;
}
model {
  int flag = 0;
  for (it in 1:1) {
    vector[0] v;
    v = [0.5, 0.75]';
    for (idx in 1:num_elements(v)) {
      real e = v[idx];
      for (i in 1:2) {
        if (e <= -0.24) {
          flag = 1;
          break;
        }
      }
      if (flag) break;
    }
    if (flag) break;
  }
  if (flag != 0) reject("flag is ", flag);
}
