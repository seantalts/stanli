// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// The breaks of three nested loops, each selected by a flag that the loop
// inside it sets. The vector is a zero-size local given its size by an
// assignment. CmdStan ends with flag 1.
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
    flag = 1;
    break;
  }
  if (flag != 1) reject("flag is ", flag);
}
