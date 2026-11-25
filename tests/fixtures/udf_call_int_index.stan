functions {
  array[,] int transpose_int(array[,] int a) {
    array[size(a[1]), size(a)] int t;
    for (i in 1:size(a)) for (j in 1:size(a[1])) t[j, i] = a[i, j];
    return t;
  }
}
data { int n; array[3, 4] int a; }
parameters { real q; }
model {
  int k = transpose_int(a)[4, n];
  target += normal_lpdf(q | k, 1);
}
