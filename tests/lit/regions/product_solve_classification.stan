// Shaped multiplication and the linear solves classified by the shared
// registry resolvers, value-pinned in the unguarded graph and inside a
// runtime-control region (the register machine's classification). The
// region checks compare with a 1e-12 band, not exact equality: the
// island's gradient pass replays the program on var, and stan-math's var
// solve overloads factor in a different association than the double
// forward, a within-ULP-policy divergence this fixture must not trip on.
parameters {
  real theta;
}
model {
  matrix[2, 3] A0 = [[1, 2, 3], [4, 5, 6]];
  vector[3] v0 = [1, 2, 3]';
  vector[2] mv0 = A0 * v0;
  row_vector[3] rm0 = [1, 2] * A0;
  if (abs(mv0[2] - 32) > 0 || abs(rm0[3] - 15) > 0)
    reject("unguarded product picked the wrong cells");
  vector[2] x0 = mdivide_left_tri_low([[2, 0], [1, 3]], [4, 11]');
  if (abs(x0[1] - 2) > 0 || abs(x0[2] - 3) > 0)
    reject("unguarded triangular solve wrong");
  row_vector[2] z0 = [7, 9] / [[2, 1], [1, 3]];
  if (abs(z0[1] - 2.4) > 1e-12 || abs(z0[2] - 2.2) > 1e-12)
    reject("unguarded right solve wrong");
  if (abs(theta) >= 0) {
    matrix[2, 3] A = [[1, 2, 3], [4, 5, 6]];
    matrix[3, 2] B = [[1, 0], [0, 1], [1, 1]];
    matrix[2, 2] P = A * B;
    if (abs(P[1, 1] - 4) > 1e-12 || abs(P[2, 2] - 11) > 1e-12)
      reject("region matrix product picked the wrong cells");
    vector[2] mv = A * [1, 2, 3]';
    row_vector[3] rm = [1, 2] * A;
    real dot = [1, 2] * mv;
    matrix[2, 3] outer_m = mv * rm;
    if (abs(mv[1] - 14) > 1e-12 || abs(rm[3] - 15) > 1e-12
        || abs(dot - 78) > 1e-12 || abs(outer_m[2, 1] - 288) > 1e-12)
      reject("region product classification wrong");
    vector[2] x = mdivide_left([[2, 0], [1, 3]], [4, 11]');
    if (abs(x[1] - 2) > 1e-12 || abs(x[2] - 3) > 1e-12)
      reject("region solve wrong");
    target += theta * (P[1, 1] + mv[2] + dot + x[2]);
  }
  target += theta ^ 2 + mv0[1] + x0[1] + z0[1];
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
