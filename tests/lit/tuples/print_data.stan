// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: a=[(1,[(1.5,[1,1]),(2,[1,2])]),(2,[(2.5,[2,1]),(3,[2,2])])] e=[] m=[([[1,2],[3,4]],7)] t=(1,[2,3])
// reject in transformed data spells arrays of tuples as stan_print does,
// nested ones and empty ones included.
transformed data {
  array[2] tuple(int, array[2] tuple(real, vector[2])) a;
  for (i in 1:2) {
    a[i].1 = i;
    for (j in 1:2) a[i].2[j] = (i + 0.5 * j, [i, j]');
  }
  array[0] tuple(int, real) e;
  array[1] tuple(matrix[2, 2], int) m = {([[1, 2], [3, 4]], 7)};
  reject("a=", a, " e=", e, " m=", m, " t=", (1, {2, 3}));
}
parameters {
  real x;
}
model {
  x ~ normal(0, 1);
}
