// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
functions {
  void expect(real got, real want, int id) {
    if (got != want) reject("case ", id, ": got ", got, ", want ", want);
  }
  real while_exit(real x, real y) {
    if (x > 0) return 2 * y;
    while (x < -1) {
      if (x < -2) return -3 * y;
      return -4 * y;
    }
    return y + 7;
  }
  real while_in_for(real y) {
    for (i in 1:3) {
      while (i < 4) {
        return 100 * i + y;
      }
    }
    return -1;
  }
  real for_in_while(real y) {
    int k = 0;
    while (k < 2) {
      for (i in 1:3) {
        if (i == 2) return 200 + y;
      }
      k += 1;
    }
    return -2;
  }
  real nested_while(real y) {
    int a = 0;
    while (a < 3) {
      int b = 0;
      while (b < 3) {
        if (b == 1) return 300 + y;
        b += 1;
      }
      a += 1;
    }
    return -3;
  }
  real break_beside_return(real y) {
    int a = 0;
    while (a < 5) {
      a += 1;
      if (a == 4) break;
      if (a == 2) return 400 + y;
    }
    return -4;
  }
  void for_exit_lp(real y) {
    for (i in 1:3) {
      if (i == 2) return;
    }
    target += 1000 * y;
  }
  void while_exit_lp(real y) {
    int a = 0;
    while (a < 3) {
      a += 1;
      if (a == 2) return;
    }
    target += 2000 * y;
  }
  void if_exit_lp(real y) {
    if (y < 1e300) return;
    target += 3000 * y;
  }
  void check_values(real y) {
    expect(while_exit(-1.6, y), -4 * y, 1);
    expect(while_exit(-2.5, y), -3 * y, 2);
    expect(while_exit(-0.5, y), y + 7, 3);
    expect(while_in_for(y), 100 + y, 4);
    expect(for_in_while(y), 200 + y, 5);
    expect(nested_while(y), 300 + y, 6);
    expect(break_beside_return(y), 400 + y, 7);
  }
}
parameters {
  real x;
}
model {
  x ~ normal(0, 1);
  check_values(x);
  real before = target();
  for_exit_lp(x);
  expect(target(), before, 8);
  while_exit_lp(x);
  expect(target(), before, 9);
  if_exit_lp(x);
  expect(target(), before, 10);
}
generated quantities {
  check_values(x);
}
