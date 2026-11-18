// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: q=[(1,[2]),(2,[3])]
// reject inside a loop whose trip count depends on a parameter.
parameters {
  real<lower=0> x;
}
model {
  array[2] tuple(real, array[1] real) q = {(x / x, {x / x + 1}), (2 * x / x, {2 * x / x + 1})};
  real s = x;
  while (s < 4) {
    s += 1;
    if (s > 3) reject("q=", q);
  }
  x ~ normal(0, 1);
}
