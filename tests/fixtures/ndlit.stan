// Deep array literals in transformed data, against data-block arrays of the
// same shape. Every element's value is its own decimal index code, so any
// permutation of the layout changes the weighted sums below.
data {
  array[2,2] real d2;
  array[2,2,2] real d3;
  array[2,2,2,2] real d4;
  array[2,2] vector[3] dvv;
  array[2] matrix[2,3] dm;
}
transformed data {
  array[2,2] real t2 = {{11.,12.},{21.,22.}};
  array[2,2,2] real t3
      = {{{111.,112.},{121.,122.}},{{211.,212.},{221.,222.}}};
  array[2,2,2,2] real t4
      = {{{{1111.,1112.},{1121.,1122.}},{{1211.,1212.},{1221.,1222.}}},
         {{{2111.,2112.},{2121.,2122.}},{{2211.,2212.},{2221.,2222.}}}};
  array[2,2] vector[3] tvv
      = {{[111.,112.,113.]',[121.,122.,123.]'},
         {[211.,212.,213.]',[221.,222.,223.]'}};
  array[2] matrix[2,3] tm
      = {[[111.,112.,113.],[121.,122.,123.]],
         [[211.,212.,213.],[221.,222.,223.]]};

  // Reading the literal back through the interpreter's own N-D indexing.
  real s3 = 0;
  for (i in 1:2) for (j in 1:2) for (k in 1:2)
    s3 += t3[i,j,k] * (100*i + 10*j + k);
}
parameters { real x; real y; real z; }
model {
  real diff = 0;
  real wt = 0;
  for (i in 1:2) for (j in 1:2) {
    diff += (t2[i,j] - d2[i,j]) * (10*i + j);
    wt += t2[i,j] * (10*i + j);
  }
  for (i in 1:2) for (j in 1:2) for (k in 1:2) {
    diff += (t3[i,j,k] - d3[i,j,k]) * (100*i + 10*j + k);
    wt += t3[i,j,k] * (100*i + 10*j + k);
  }
  for (i in 1:2) for (j in 1:2) for (k in 1:2) for (l in 1:2) {
    diff += (t4[i,j,k,l] - d4[i,j,k,l]) * (1000*i + 100*j + 10*k + l);
    wt += t4[i,j,k,l] * (1000*i + 100*j + 10*k + l);
  }
  for (i in 1:2) for (j in 1:2) for (k in 1:3) {
    diff += (tvv[i,j,k] - dvv[i,j,k]) * (100*i + 10*j + k);
    wt += tvv[i,j,k] * (100*i + 10*j + k);
  }
  for (i in 1:2) for (j in 1:2) for (k in 1:3) {
    diff += (tm[i,j,k] - dm[i,j,k]) * (100*i + 10*j + k);
    wt += tm[i,j,k] * (100*i + 10*j + k);
  }
  target += diff * x + wt * y + s3 * z;
}
