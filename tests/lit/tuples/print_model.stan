// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: p=[[(0.5,[1,2],[[1,3],[2,4]]),(1,[3,4],[[1,3],[2,4]])],[(1.5,[5,6],[[1,3],[2,4]]),(2,[7,8],[[1,3],[2,4]])]]
// reject in the model block, where the array of tuples depends on a
// parameter, evaluated on the graph.
parameters {
  real<lower=0> x;
}
model {
  array[2, 2] tuple(real, array[2] int, matrix[2, 2]) p;
  for (i in 1:2)
    for (j in 1:2)
      p[i, j] = ((x / x) * (2 * i + j - 2) / 2,
                 {4 * i + 2 * j - 5, 4 * i + 2 * j - 4},
                 (x / x) * [[1, 3], [2, 4]]);
  x ~ normal(0, 1);
  reject("p=", p);
}
