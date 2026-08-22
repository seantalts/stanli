functions {
  matrix ident(matrix x) {
    return x;
  }
}
parameters {
  matrix[5, 10] a;
  matrix[5, 10] b;
  matrix[5, 10] c;
}
transformed parameters {
  matrix[5, 10] mix = sum(a) > 0 ? ident(b + c) : c;
}
model {
  target += sum(mix);
}
