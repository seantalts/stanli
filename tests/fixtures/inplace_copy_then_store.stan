// Each block copies a container, writes the copy, then writes the original
// in place. The copy must keep the value that the original held when it was
// copied.
parameters {
  real x;
  real y1;
  real y2;
  real y3;
  real y4;
  real y5;
  real z;
}
model {
  vector[3] v = rep_vector(0, 3);
  v[2] = x;
  v[1] = y1;
  vector[3] w = v;
  w[3] = 1;
  v[1] = z;
  target += normal_lpdf(w | 0, 1) + normal_lpdf(v | 0, 1);

  vector[3] a = rep_vector(0, 3);
  a[2] = x;
  a[1] = y2;
  vector[3] b = a;
  b[2:3] = [1, 2]';
  a[1] = z;
  target += normal_lpdf(b | 0, 1) + normal_lpdf(a | 0, 1);

  vector[3] c = rep_vector(0, 3);
  c[2] = x;
  c[1] = y3;
  vector[4] u = rep_vector(0, 4);
  u[2:4] = c;
  c[1] = z;
  target += normal_lpdf(u | 0, 1) + normal_lpdf(c | 0, 1);

  matrix[2, 2] m = rep_matrix(0, 2, 2);
  m[1, 2] = x;
  m[1, 1] = y4;
  matrix[2, 2] m2 = m;
  m2[2, 2] = 1;
  m[1, 1] = z;
  target += normal_lpdf(to_vector(m2) | 0, 1)
            + normal_lpdf(to_vector(m) | 0, 1);

  array[3] real r = rep_array(0.0, 3);
  r[2] = x;
  r[1] = y5;
  array[3] real r2 = r;
  r2[3] = 1;
  r[1] = z;
  target += normal_lpdf(r2 | 0, 1) + normal_lpdf(r | 0, 1);
}
