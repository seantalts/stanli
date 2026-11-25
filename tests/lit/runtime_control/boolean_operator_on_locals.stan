// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// && and || whose operands read a function's local, which is not data,
// passed as an argument. Each check rejects when the value is wrong. The
// overload keeps stanc from inlining flags.
functions {
  void expect_true(int ok, int id) {
    if (!ok) reject("case ", id, " is false");
  }
  void flags(real mu) {
    array[2] real a = {1.5, 2.5};
    expect_true(a[1] == 1.5 && a[2] == 2.5, 1);
    expect_true(a[1] == 2.5 || a[2] == 2.5, 2);
    expect_true(!(a[1] == 2.5 && a[2] == 2.5), 3);
    expect_true(a[1] < mu || a[2] > mu, 4);
  }
  void flags(vector mu) {
  }
}
parameters {
  real<lower=0, upper=1> mu;
}
model {
  flags(mu);
  mu ~ beta(2, 2);
}
generated quantities {
  flags(0.5);
}
