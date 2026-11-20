// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// A branch on a parameter compiles to a register program. Stan evaluates the
// whole right-hand side before it assigns, so a right side that reads the
// cells being written must see their old values.
parameters {
  real theta;
}
model {
  vector[4] x = [1, 2, 3, 4]';
  vector[4] y = [1, 2, 3, 4]';
  vector[3] v = [1, 2, 3]';
  row_vector[3] r = [1, 2, 3];
  array[4] real a = {1, 2, 3, 4};
  array[3] real b = {1, 2, 3};
  matrix[2, 2] m = [[1, 2], [3, 4]];
  matrix[2, 2] n = [[1, 2], [3, 4]];
  array[3] vector[2] c = {[1, 2]', [3, 4]', [5, 6]'};
  if (theta > -100) {
    x[2:4] = x[1:3];
    y[2:] = y[1:3];
    v[{3, 2, 1}] = v;
    r[{3, 2, 1}] = r;
    a[2:4] = a[1:3];
    b[{3, 2, 1}] = b;
    m[{2, 1}, :] = m;
    n[:, {2, 1}] = n;
    c[2:3] = c[1:2];
  }
  if (x[1] != 1 || x[2] != 1 || x[3] != 2 || x[4] != 3)
    reject("x[2:4] = x[1:3] gave ", x);
  if (y[1] != 1 || y[2] != 1 || y[3] != 2 || y[4] != 3)
    reject("y[2:] = y[1:3] gave ", y);
  if (v[1] != 3 || v[2] != 2 || v[3] != 1)
    reject("v[{3, 2, 1}] = v gave ", v);
  if (r[1] != 3 || r[2] != 2 || r[3] != 1)
    reject("r[{3, 2, 1}] = r gave ", r);
  if (a[1] != 1 || a[2] != 1 || a[3] != 2 || a[4] != 3)
    reject("a[2:4] = a[1:3] gave ", a);
  if (b[1] != 3 || b[2] != 2 || b[3] != 1)
    reject("b[{3, 2, 1}] = b gave ", b);
  if (m[1, 1] != 3 || m[1, 2] != 4 || m[2, 1] != 1 || m[2, 2] != 2)
    reject("m[{2, 1}, :] = m gave ", m);
  if (n[1, 1] != 2 || n[1, 2] != 1 || n[2, 1] != 4 || n[2, 2] != 3)
    reject("n[:, {2, 1}] = n gave ", n);
  if (c[1, 1] != 1 || c[2, 1] != 1 || c[2, 2] != 2 || c[3, 1] != 3 || c[3, 2] != 4)
    reject("c[2:3] = c[1:2] gave ", c);
  theta ~ normal(0, 1);
}
